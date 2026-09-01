// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_run_state.h"
#include "sak/ai/ai_run_state_store.h"

#include <QBuffer>
#include <QFile>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <algorithm>
#include <cstring>

namespace {

constexpr qsizetype kOversizedRunStateMessageBytes = 16 * 1024 * 1024;

class PartialThenFailDevice final : public QIODevice {
public:
    explicit PartialThenFailDevice(QByteArray prefix) : m_prefix(std::move(prefix)) {}

    bool isSequential() const override { return true; }

protected:
    qint64 readData(char* data, qint64 max_size) override {
        if (m_offset < m_prefix.size()) {
            const qint64 count = std::min(max_size, m_prefix.size() - m_offset);
            std::memcpy(data, m_prefix.constData() + m_offset, static_cast<size_t>(count));
            m_offset += count;
            return count;
        }
        setErrorString(QStringLiteral("synthetic read failure"));
        return -1;
    }

    qint64 writeData(const char*, qint64) override { return -1; }

private:
    QByteArray m_prefix;
    qint64 m_offset{0};
};

}  // namespace

class AiRunStateStoreTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void emptySessionDirectoryReportsError();
    void saveAndLoadRoundTrips();
    void clearRemovesSnapshot();
    void loadWithoutSnapshotReturnsDefault();
    void saveRejectsOversizedPayload();
    void saveReportsDirectoryCreationFailure();
    void loadRejectsOversizedFile();
    void boundedReadRejectsGrowthBeyondCap();
    void boundedReadRejectsErrorAfterValidPrefix();
    void loadRejectsMissingSchemaVersion();
    void loadRejectsWrongFieldType();
    void loadRejectsUnknownStatus();
    void loadRejectsNonObjectRoot();
    void loadReportsJsonParseError();

    static QJsonObject validSnapshotObject();
    static void writeSnapshot(const QString& path, const QByteArray& bytes);
};

QJsonObject AiRunStateStoreTests::validSnapshotObject() {
    sak::ai::AiRunState state;
    state.run_id = QStringLiteral("run_valid");
    state.status = sak::ai::AiRunStatus::Running;
    QJsonObject object = state.toJson();
    object[QStringLiteral("schema_version")] = 1;
    return object;
}

void AiRunStateStoreTests::writeSnapshot(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Truncate), qPrintable(file.errorString()));
    QCOMPARE(file.write(bytes), static_cast<qint64>(bytes.size()));
}

void AiRunStateStoreTests::emptySessionDirectoryReportsError() {
    sak::ai::AiRunStateStore store;
    QString error;
    QVERIFY(!store.saveSnapshot({}, &error));
    QCOMPARE(error, QStringLiteral("Run-state store has no session directory"));
    QVERIFY(store.runStatePath().isEmpty());
}

void AiRunStateStoreTests::saveAndLoadRoundTrips() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    sak::ai::AiRunStateStore store(temp_dir.path());

    sak::ai::AiRunState state;
    state.run_id = QStringLiteral("run_abc");
    state.workflow_id = QStringLiteral("drive_health_deep_check");
    state.status = sak::ai::AiRunStatus::Running;
    state.phase_id = QStringLiteral("collect_evidence");
    state.active_subagents = 1;
    state.completed_subagents = 2;
    state.active_tools = 3;
    state.completed_tools = 4;
    state.message = QStringLiteral("Inspecting drive");

    QString error;
    QVERIFY(store.saveSnapshot(state, &error));
    QVERIFY(error.isEmpty());
    QVERIFY(QFileInfo(store.runStatePath()).exists());

    QFile snapshot(store.runStatePath());
    QVERIFY(snapshot.open(QIODevice::ReadOnly));
    const QJsonObject saved = QJsonDocument::fromJson(snapshot.readAll()).object();
    QCOMPARE(saved.value(QStringLiteral("schema_version")).toInt(), 1);

    const auto loaded = store.loadSnapshot(&error);
    QVERIFY(error.isEmpty());
    QCOMPARE(loaded.run_id, state.run_id);
    QCOMPARE(loaded.workflow_id, state.workflow_id);
    QCOMPARE(loaded.status, state.status);
    QCOMPARE(loaded.phase_id, state.phase_id);
    QCOMPARE(loaded.active_subagents, state.active_subagents);
    QCOMPARE(loaded.completed_subagents, state.completed_subagents);
    QCOMPARE(loaded.active_tools, state.active_tools);
    QCOMPARE(loaded.completed_tools, state.completed_tools);
    QCOMPARE(loaded.message, state.message);
}

void AiRunStateStoreTests::clearRemovesSnapshot() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    sak::ai::AiRunStateStore store(temp_dir.path());
    sak::ai::AiRunState state;
    state.run_id = QStringLiteral("run_xyz");
    state.status = sak::ai::AiRunStatus::Completed;

    QVERIFY(store.saveSnapshot(state));
    QVERIFY(QFileInfo(store.runStatePath()).exists());

    QString error;
    QVERIFY(store.clearSnapshot(&error));
    QVERIFY(error.isEmpty());
    QVERIFY(!QFileInfo(store.runStatePath()).exists());
    // Clearing again is a no-op.
    QVERIFY(store.clearSnapshot(&error));
}

void AiRunStateStoreTests::loadWithoutSnapshotReturnsDefault() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());
    sak::ai::AiRunStateStore store(temp_dir.path());
    QString error;
    const auto state = store.loadSnapshot(&error);
    QVERIFY(error.isEmpty());
    QCOMPARE(state.status, sak::ai::AiRunStatus::Idle);
    QVERIFY(state.run_id.isEmpty());
}

void AiRunStateStoreTests::saveRejectsOversizedPayload() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());
    sak::ai::AiRunStateStore store(temp_dir.path());
    sak::ai::AiRunState state;
    state.message = QString(kOversizedRunStateMessageBytes, QLatin1Char('x'));

    QString error;
    QVERIFY(!store.saveSnapshot(state, &error));
    QCOMPARE(error, QStringLiteral("Cannot save run-state snapshot: encoded payload is too large"));
    QVERIFY(!QFileInfo::exists(store.runStatePath()));
}

void AiRunStateStoreTests::saveReportsDirectoryCreationFailure() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());
    const QString blocker = QDir(temp_dir.path()).filePath(QStringLiteral("not-a-directory"));
    writeSnapshot(blocker, QByteArrayLiteral("block"));
    const QString session_dir = QDir(blocker).filePath(QStringLiteral("session"));
    sak::ai::AiRunStateStore store(session_dir);

    QString error;
    QVERIFY(!store.saveSnapshot({}, &error));
    QCOMPARE(error, QStringLiteral("Could not create run-state directory: %1").arg(session_dir));
}

void AiRunStateStoreTests::loadRejectsOversizedFile() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());
    sak::ai::AiRunStateStore store(temp_dir.path());
    writeSnapshot(store.runStatePath(),
                  QByteArray(kOversizedRunStateMessageBytes + 1, static_cast<char>('x')));

    QString error;
    const auto state = store.loadSnapshot(&error);
    QCOMPARE(state.status, sak::ai::AiRunStatus::Idle);
    QCOMPARE(error, QStringLiteral("Run-state snapshot declared size exceeds the read limit"));
}

void AiRunStateStoreTests::boundedReadRejectsGrowthBeyondCap() {
    QByteArray oversized(kOversizedRunStateMessageBytes + 1, static_cast<char>('x'));
    QBuffer device(&oversized);
    QVERIFY(device.open(QIODevice::ReadOnly));
    QByteArray bytes;
    QString error;
    QVERIFY(!sak::ai::AiRunStateStore::readBoundedSnapshot(&device, &bytes, &error));
    QCOMPARE(error, QStringLiteral("Run-state snapshot grew beyond the read size limit"));
    QCOMPARE(bytes.size(), kOversizedRunStateMessageBytes + 1);
}

void AiRunStateStoreTests::boundedReadRejectsErrorAfterValidPrefix() {
    PartialThenFailDevice device(QByteArrayLiteral("{}"));
    QVERIFY(device.open(QIODevice::ReadOnly));
    QByteArray bytes;
    QString error;
    QVERIFY(!sak::ai::AiRunStateStore::readBoundedSnapshot(&device, &bytes, &error));
    QCOMPARE(error, QStringLiteral("Run-state snapshot read failed: synthetic read failure"));
}

void AiRunStateStoreTests::loadRejectsMissingSchemaVersion() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());
    sak::ai::AiRunStateStore store(temp_dir.path());
    QJsonObject object = validSnapshotObject();
    object.remove(QStringLiteral("schema_version"));
    writeSnapshot(store.runStatePath(), QJsonDocument(object).toJson(QJsonDocument::Compact));

    QString error;
    (void)store.loadSnapshot(&error);
    QCOMPARE(error, QStringLiteral("Run-state snapshot has unsupported or missing schema_version"));
}

void AiRunStateStoreTests::loadRejectsWrongFieldType() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());
    sak::ai::AiRunStateStore store(temp_dir.path());
    QJsonObject object = validSnapshotObject();
    object[QStringLiteral("run_id")] = 7;
    writeSnapshot(store.runStatePath(), QJsonDocument(object).toJson(QJsonDocument::Compact));

    QString error;
    (void)store.loadSnapshot(&error);
    QCOMPARE(error, QStringLiteral("Run-state snapshot field 'run_id' must be a string"));
}

void AiRunStateStoreTests::loadRejectsUnknownStatus() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());
    sak::ai::AiRunStateStore store(temp_dir.path());
    QJsonObject object = validSnapshotObject();
    object[QStringLiteral("status")] = QStringLiteral("mystery");
    writeSnapshot(store.runStatePath(), QJsonDocument(object).toJson(QJsonDocument::Compact));

    QString error;
    (void)store.loadSnapshot(&error);
    QCOMPARE(error, QStringLiteral("Run-state snapshot has unknown status: mystery"));
}

void AiRunStateStoreTests::loadRejectsNonObjectRoot() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());
    sak::ai::AiRunStateStore store(temp_dir.path());
    writeSnapshot(store.runStatePath(), QByteArrayLiteral("[]"));

    QString error;
    (void)store.loadSnapshot(&error);
    QCOMPARE(error, QStringLiteral("Run-state snapshot root must be a JSON object"));
}

void AiRunStateStoreTests::loadReportsJsonParseError() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());
    sak::ai::AiRunStateStore store(temp_dir.path());
    const QByteArray malformed = QByteArrayLiteral("{\"run_id\":");
    writeSnapshot(store.runStatePath(), malformed);
    QJsonParseError expected_parse_error{};
    (void)QJsonDocument::fromJson(malformed, &expected_parse_error);

    QString error;
    (void)store.loadSnapshot(&error);
    QCOMPARE(error,
             QStringLiteral("Run-state snapshot JSON parse failed: %1")
                 .arg(expected_parse_error.errorString()));
}

QTEST_GUILESS_MAIN(AiRunStateStoreTests)
#include "test_ai_run_state_store.moc"
