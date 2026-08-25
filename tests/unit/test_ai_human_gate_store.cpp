// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_human_gate_store.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class AiHumanGateStoreTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void appendAndLoadRoundTrips();
    void persistedRecordCarriesTheDocumentedKeys();
    void appendGatePreservesCallerSuppliedTimestamps();
    void latestPendingGateIgnoresResolvedGates();
    void latestPendingGateKeysOnGateIdAndPrefersTheNewest();
    void everyResolvingStatusRetiresAPendingGate();
    void latestPendingGateIgnoresStatuslessForgedRecord();
    void forgedStatusNearMissesDoNotResolveAGate();
};

namespace {

/// The one gate shape the pending/forged tests share.
sak::ai::AiHumanGate makeGate(const QString& gate_id, const QString& run_id) {
    sak::ai::AiHumanGate gate;
    gate.gate_id = gate_id;
    gate.run_id = run_id;
    gate.kind = QStringLiteral("approval");
    gate.name = QStringLiteral("command_approval");
    gate.status = sak::ai::humanGateWaitingStatus();
    gate.question = QStringLiteral("Approve command?");
    return gate;
}

}  // namespace

void AiHumanGateStoreTests::appendAndLoadRoundTrips() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    sak::ai::AiHumanGateStore store(temp_dir.path());
    sak::ai::AiHumanGate gate;
    gate.gate_id = QStringLiteral("gate_approval_1");
    gate.run_id = QStringLiteral("run_1");
    gate.workflow_id = QStringLiteral("install_app_now");
    gate.phase_id = QStringLiteral("install");
    gate.kind = QStringLiteral("approval");
    gate.name = QStringLiteral("command_approval");
    gate.status = sak::ai::humanGateWaitingStatus();
    gate.question = QStringLiteral("Approve command?");
    gate.metadata[QStringLiteral("preview")] = QStringLiteral("choco install test");

    QString error;
    QVERIFY2(store.appendGate(gate, &error), qPrintable(error));

    const auto gates = store.loadGates(&error);
    // The out-param is not cosmetic and was never asserted empty after a SUCCESSFUL load. The
    // panel treats any non-empty error as fatal and returns WITHOUT restoring the pending gate,
    // so a load that reported a phantom "unreadable record(s)" would lose every pending gate at
    // startup while every QCOMPARE in this file still passed.
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(gates.size(), 1);
    // Eight fields are populated above and three were inspected, so a round trip that dropped
    // workflow_id / phase_id / kind / name / question passed.
    const auto& loaded = gates.first();
    QCOMPARE(loaded.gate_id, QStringLiteral("gate_approval_1"));
    QCOMPARE(loaded.run_id, QStringLiteral("run_1"));
    QCOMPARE(loaded.workflow_id, QStringLiteral("install_app_now"));
    QCOMPARE(loaded.phase_id, QStringLiteral("install"));
    QCOMPARE(loaded.kind, QStringLiteral("approval"));
    QCOMPARE(loaded.name, QStringLiteral("command_approval"));
    // Pin the persisted WIRE value, not only the accessor: a human_gates.jsonl written by an
    // older build must keep parsing.
    QCOMPARE(loaded.status, QStringLiteral("waiting_for_human"));
    QCOMPARE(loaded.status, sak::ai::humanGateWaitingStatus());
    QCOMPARE(loaded.question, QStringLiteral("Approve command?"));
    QCOMPARE(loaded.decision, QString());
    QCOMPARE(loaded.response_summary, QString());
    QCOMPARE(loaded.metadata.size(), 1);
    QCOMPARE(loaded.metadata.value(QStringLiteral("preview")).toString(),
             QStringLiteral("choco install test"));
    // appendGate stamps created_utc and leaves resolved_utc unset while the gate is pending.
    QVERIFY(loaded.created_utc.isValid());
    QVERIFY(!loaded.resolved_utc.isValid());
    // The existence check was mirror-vacuous: the writer and the check used the same accessor,
    // so the log could have landed anywhere. Pin the NAME and the LOCATION.
    QCOMPARE(store.gateLogPath(), temp_dir.path() + QStringLiteral("/human_gates.jsonl"));
    QVERIFY(QFile::exists(temp_dir.path() + QStringLiteral("/human_gates.jsonl")));
}

void AiHumanGateStoreTests::persistedRecordCarriesTheDocumentedKeys() {
    // The log was proved to EXIST at the right path, and nothing ever looked inside it. Every
    // field assertion above travels appendGate -> toJson -> fromJson -> accessor, i.e. the SAME
    // symmetric pair, so any key renamed on both sides survives untouched. human_gates.jsonl is a
    // cross-build contract -- an older build's log must keep parsing -- so the on-disk KEY NAMES
    // are the thing to pin, and they were pinned nowhere.
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());
    sak::ai::AiHumanGateStore store(temp_dir.path());

    sak::ai::AiHumanGate gate = makeGate(QStringLiteral("gate_keys"), QStringLiteral("run_keys"));
    gate.workflow_id = QStringLiteral("install_app_now");
    gate.phase_id = QStringLiteral("install");
    gate.metadata[QStringLiteral("preview")] = QStringLiteral("choco install test");

    QString error;
    QVERIFY2(store.appendGate(gate, &error), qPrintable(error));

    QFile log(store.gateLogPath());
    QVERIFY(log.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray raw = log.readAll();
    log.close();
    // One record, one line, newline-terminated: the JSONL framing the loader depends on.
    QCOMPARE(raw.count('\n'), 1);
    QVERIFY(raw.endsWith('\n'));

    QJsonParseError parse_error{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw.trimmed(), &parse_error);
    QVERIFY2(parse_error.error == QJsonParseError::NoError, qPrintable(parse_error.errorString()));
    QVERIFY(doc.isObject());
    const QJsonObject object = doc.object();

    const QSet<QString> expected_keys{QStringLiteral("gate_id"),
                                      QStringLiteral("run_id"),
                                      QStringLiteral("workflow_id"),
                                      QStringLiteral("phase_id"),
                                      QStringLiteral("kind"),
                                      QStringLiteral("name"),
                                      QStringLiteral("status"),
                                      QStringLiteral("question"),
                                      QStringLiteral("decision"),
                                      QStringLiteral("response_summary"),
                                      QStringLiteral("created_utc"),
                                      QStringLiteral("resolved_utc"),
                                      QStringLiteral("metadata")};
    const QStringList actual_keys = object.keys();
    QCOMPARE(QSet<QString>(actual_keys.begin(), actual_keys.end()), expected_keys);

    // ... and the values land under the keys they name, so a pair of keys cannot be swapped.
    QCOMPARE(object.value(QStringLiteral("gate_id")).toString(), QStringLiteral("gate_keys"));
    QCOMPARE(object.value(QStringLiteral("run_id")).toString(), QStringLiteral("run_keys"));
    QCOMPARE(object.value(QStringLiteral("workflow_id")).toString(),
             QStringLiteral("install_app_now"));
    QCOMPARE(object.value(QStringLiteral("status")).toString(),
             QStringLiteral("waiting_for_human"));
    QCOMPARE(object.value(QStringLiteral("metadata"))
                 .toObject()
                 .value(QStringLiteral("preview"))
                 .toString(),
             QStringLiteral("choco install test"));
    // A pending gate carries an empty resolved_utc on the wire, not a missing key.
    QVERIFY(object.contains(QStringLiteral("resolved_utc")));
    QCOMPARE(object.value(QStringLiteral("resolved_utc")).toString(), QString());
}

void AiHumanGateStoreTests::appendGatePreservesCallerSuppliedTimestamps() {
    // appendGate stamps created_utc only if the caller left it unset, and resolved_utc only if
    // the gate is non-pending AND the caller left it unset. Every fixture in this file leaves
    // both unset, so only the AUTO-STAMP arm was ever taken -- while production relies on the
    // PRESERVE arm, since the panel supplies the real creation time and the real decision time.
    // Overwriting a caller's timestamp would silently rewrite the audit history.
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());
    sak::ai::AiHumanGateStore store(temp_dir.path());

    const QDateTime created(QDate(2026, 3, 4), QTime(5, 6, 7, 8), QTimeZone::UTC);
    const QDateTime resolved(QDate(2026, 3, 4), QTime(9, 10, 11, 12), QTimeZone::UTC);

    sak::ai::AiHumanGate gate = makeGate(QStringLiteral("gate_ts"), QStringLiteral("run_ts"));
    gate.status = sak::ai::humanGateApprovedStatus();  // non-pending, so both arms are in play
    gate.created_utc = created;
    gate.resolved_utc = resolved;

    QString error;
    QVERIFY2(store.appendGate(gate, &error), qPrintable(error));
    const auto gates = store.loadGates(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(gates.size(), 1);
    // Exact values, to the millisecond: the serializer is ISODateWithMs, so a precision loss
    // here would silently coarsen every timestamp in the audit log.
    QCOMPARE(gates.first().created_utc, created);
    QCOMPARE(gates.first().resolved_utc, resolved);
}

void AiHumanGateStoreTests::latestPendingGateIgnoresResolvedGates() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    sak::ai::AiHumanGateStore store(temp_dir.path());

    sak::ai::AiHumanGate first;
    first.gate_id = QStringLiteral("gate_1");
    first.run_id = QStringLiteral("run_1");
    first.kind = QStringLiteral("workflow_input");
    first.name = QStringLiteral("required_input");
    first.status = sak::ai::humanGateWaitingStatus();
    first.question = QStringLiteral("Which app?");

    sak::ai::AiHumanGate first_resolved = first;
    first_resolved.status = sak::ai::humanGateCompletedStatus();
    first_resolved.decision = QStringLiteral("provided");
    first_resolved.response_summary = QStringLiteral("Firefox");

    sak::ai::AiHumanGate second;
    second.gate_id = QStringLiteral("gate_2");
    second.run_id = QStringLiteral("run_2");
    second.kind = QStringLiteral("approval");
    second.name = QStringLiteral("restore_point_offer");
    second.status = sak::ai::humanGateWaitingStatus();
    second.question = QStringLiteral("Create restore point?");

    // ORDER IS THE WHOLE POINT, and the old order made this test's own claim unreachable. With
    // the log written as [gate_1 waiting, gate_1 completed, gate_2 waiting], an implementation
    // that ignored the per-gate_id latest-state map entirely and simply returned the last record
    // whose OWN status is waiting_for_human answered gate_2 too -- gate_1's stale pending record
    // sat before gate_2's and was never reached, so the resolved gate was never actually proved
    // to be ignored. Writing gate_2 FIRST puts gate_1's stale pending record LATER in the log,
    // where that naive implementation returns gate_1 and the real one still returns gate_2.
    QString error;
    QVERIFY2(store.appendGate(second, &error), qPrintable(error));
    QVERIFY2(store.appendGate(first, &error), qPrintable(error));
    QVERIFY2(store.appendGate(first_resolved, &error), qPrintable(error));

    const auto pending = store.latestPendingGate(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(pending.gate_id, QStringLiteral("gate_2"));
    QCOMPARE(pending.run_id, QStringLiteral("run_2"));
    QCOMPARE(pending.kind, QStringLiteral("approval"));
    QCOMPARE(pending.name, QStringLiteral("restore_point_offer"));
    QCOMPARE(pending.status, sak::ai::humanGateWaitingStatus());
    QCOMPARE(pending.question, QStringLiteral("Create restore point?"));
    // The winner must be the PENDING record itself, not resolved state leaking through: a
    // resolved record carries decision / response_summary / resolved_utc.
    QCOMPARE(pending.decision, QString());
    QCOMPARE(pending.response_summary, QString());
    QVERIFY(!pending.resolved_utc.isValid());
    // All three records stay in the append-only log, in order. loadGates must not drop the
    // resolved one, or gate_1's stale pending record becomes its latest state and an
    // already-approved gate is re-raised on the next start.
    const auto all_records = store.loadGates(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(all_records.size(), 3);
    QCOMPARE(all_records.at(0).gate_id, QStringLiteral("gate_2"));
    QCOMPARE(all_records.at(0).status, sak::ai::humanGateWaitingStatus());
    QCOMPARE(all_records.at(1).gate_id, QStringLiteral("gate_1"));
    QCOMPARE(all_records.at(1).status, sak::ai::humanGateWaitingStatus());
    QCOMPARE(all_records.at(2).gate_id, QStringLiteral("gate_1"));
    QCOMPARE(all_records.at(2).status, sak::ai::humanGateCompletedStatus());
    QCOMPARE(all_records.at(2).decision, QStringLiteral("provided"));
    QCOMPARE(all_records.at(2).response_summary, QStringLiteral("Firefox"));
    QVERIFY(all_records.at(2).resolved_utc.isValid());
}

void AiHumanGateStoreTests::latestPendingGateKeysOnGateIdAndPrefersTheNewest() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());
    sak::ai::AiHumanGateStore store(temp_dir.path());

    // Both gates share ONE run_id, which is what production actually looks like: beginHumanGate
    // stamps every gate of a run with the same run_id, so a run routinely holds several distinct
    // gate_ids under one run_id. Every fixture in this file paired the two identities 1:1, so no
    // assertion could tell which of them keyed the latest-state map.
    const QString shared_run = QStringLiteral("run_shared");
    sak::ai::AiHumanGate gate_a = makeGate(QStringLiteral("gate_a"), shared_run);
    sak::ai::AiHumanGate gate_b = makeGate(QStringLiteral("gate_b"), shared_run);
    gate_b.question = QStringLiteral("Second question?");

    QString error;
    QVERIFY2(store.appendGate(gate_a, &error), qPrintable(error));
    QVERIFY2(store.appendGate(gate_b, &error), qPrintable(error));

    // TWO gates pending at once -- the only arrangement in which the scan's DIRECTION is
    // observable. latestPendingGate scans backwards precisely so the NEWEST unanswered gate wins;
    // with a single pending gate the forward and reverse scans return the same record, so
    // "latest" -- the entire meaning of the function's name -- was unpinned.
    const auto newest = store.latestPendingGate(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(newest.gate_id, QStringLiteral("gate_b"));
    QCOMPARE(newest.question, QStringLiteral("Second question?"));

    // Now resolve the OLDER gate. Keyed on gate_id (correct) the answer is still gate_b; keyed on
    // run_id the single shared entry would now read "completed" and latestPendingGate would
    // return an empty gate, losing a genuinely pending approval.
    sak::ai::AiHumanGate gate_a_done = gate_a;
    gate_a_done.status = sak::ai::humanGateApprovedStatus();
    gate_a_done.decision = QStringLiteral("approved");
    QVERIFY2(store.appendGate(gate_a_done, &error), qPrintable(error));

    const auto still_pending = store.latestPendingGate(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(still_pending.gate_id, QStringLiteral("gate_b"));
    QVERIFY(still_pending.isPending());
}

void AiHumanGateStoreTests::everyResolvingStatusRetiresAPendingGate() {
    // isKnownHumanGateStatus is a seven-arm alternation and it is the ONLY thing that lets a
    // record resolve a pending gate. This file exercised exactly two arms -- and "completed",
    // the one it used, is the single resolving status NO production caller ever writes: every
    // resolveHumanGate call site passes approved / rejected / cancelled / skipped. The five arms
    // the shipped app actually depends on were claimed by no fixture in the repository, so any
    // of them could be dropped from the alternation and a real resolved gate would be re-raised
    // to the technician on the next start.
    const QStringList resolving{sak::ai::humanGateCompletedStatus(),
                                sak::ai::humanGateApprovedStatus(),
                                sak::ai::humanGateRejectedStatus(),
                                sak::ai::humanGateCancelledStatus(),
                                sak::ai::humanGateSkippedStatus(),
                                sak::ai::humanGateFailedStatus()};

    for (const QString& status : resolving) {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());
        sak::ai::AiHumanGateStore store(temp_dir.path());

        const sak::ai::AiHumanGate pending = makeGate(QStringLiteral("gate_x"),
                                                      QStringLiteral("run_x"));
        sak::ai::AiHumanGate resolved = pending;
        resolved.status = status;

        QString error;
        QVERIFY2(store.appendGate(pending, &error), qPrintable(error));
        // Control: it really is pending until the resolving record lands.
        QCOMPARE(store.latestPendingGate(&error).gate_id, QStringLiteral("gate_x"));
        QVERIFY2(store.appendGate(resolved, &error), qPrintable(error));

        const auto latest = store.latestPendingGate(&error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY2(latest.gate_id.isEmpty(),
                 qPrintable(
                     QStringLiteral("status '%1' failed to retire the pending gate").arg(status)));
    }
}

void AiHumanGateStoreTests::latestPendingGateIgnoresStatuslessForgedRecord() {
    // A later record for the same gate_id whose status is unrecognized/statusless must NOT
    // resolve (erase) a genuine pending gate: it is malformed and is ignored, so the gate
    // stays pending. Regression for the forged-resolving-record hardening.
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());
    sak::ai::AiHumanGateStore store(temp_dir.path());

    sak::ai::AiHumanGate pending;
    pending.gate_id = QStringLiteral("gate_forge");
    pending.run_id = QStringLiteral("run_1");
    pending.kind = QStringLiteral("approval");
    pending.name = QStringLiteral("command_approval");
    pending.status = sak::ai::humanGateWaitingStatus();
    pending.question = QStringLiteral("Approve command?");

    sak::ai::AiHumanGate forged = pending;
    forged.status = QStringLiteral("bogus_state");  // not a known lifecycle status

    QString error;
    QVERIFY2(store.appendGate(pending, &error), qPrintable(error));
    QVERIFY2(store.appendGate(forged, &error), qPrintable(error));

    const auto latest = store.latestPendingGate(&error);
    QCOMPARE(latest.gate_id, QStringLiteral("gate_forge"));
    QVERIFY(latest.isPending());
    QCOMPARE(latest.status, sak::ai::humanGateWaitingStatus());
    QCOMPARE(latest.question, QStringLiteral("Approve command?"));
    QCOMPARE(latest.kind, QStringLiteral("approval"));
    QCOMPARE(latest.name, QStringLiteral("command_approval"));
    QCOMPARE(latest.run_id, QStringLiteral("run_1"));
    // "A pending gate survived" is reachable through THREE guards; this test is named for only
    // one of them (the known-status filter in latestPendingGate). The forged record must remain
    // in the log verbatim, so the test cannot pass merely because appendGate refused to write
    // it or the parser dropped it on load -- both of which would silently truncate the audit
    // history instead.
    const auto records = store.loadGates(&error);
    QCOMPARE(records.size(), 2);
    QCOMPARE(records.at(0).gate_id, QStringLiteral("gate_forge"));
    QCOMPARE(records.at(0).status, sak::ai::humanGateWaitingStatus());
    QCOMPARE(records.at(1).gate_id, QStringLiteral("gate_forge"));
    QCOMPARE(records.at(1).status, QStringLiteral("bogus_state"));
}

void AiHumanGateStoreTests::forgedStatusNearMissesDoNotResolveAGate() {
    // The forged-record hardening rests entirely on isKnownHumanGateStatus REFUSING the status,
    // and the only refused token in the repository was "bogus_state" -- which shares no prefix,
    // no suffix and no substring with any lifecycle status, so every loosened form of those
    // compares still refuses it. These are near misses in all three directions, plus the EMPTY
    // status the header comment calls out first and which the sibling test never actually feeds.
    const QStringList forged{
        QString(),                              // empty: the case named first in the contract
        QStringLiteral(""),                     // ... and its non-null spelling
        QStringLiteral("waiting"),              // truncation of waiting_for_human
        QStringLiteral("waiting_for_humans"),   // extension
        QStringLiteral("x_waiting_for_human"),  // embedding
        QStringLiteral("WAITING_FOR_HUMAN"),    // case: these compares are byte-exact
        QStringLiteral("approve"),              // truncation of approved
        QStringLiteral("approved "),            // trailing space
        QStringLiteral("Completed"),            // case
        QStringLiteral("complete"),             // truncation of completed
    };

    for (const QString& status : forged) {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());
        sak::ai::AiHumanGateStore store(temp_dir.path());

        const sak::ai::AiHumanGate pending = makeGate(QStringLiteral("gate_forge"),
                                                      QStringLiteral("run_1"));
        sak::ai::AiHumanGate forged_record = pending;
        forged_record.status = status;

        QString error;
        QVERIFY2(store.appendGate(pending, &error), qPrintable(error));
        QVERIFY2(store.appendGate(forged_record, &error), qPrintable(error));

        const auto latest = store.latestPendingGate(&error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY2(latest.gate_id == QStringLiteral("gate_forge") && latest.isPending(),
                 qPrintable(QStringLiteral("a record with status '%1' resolved a genuine "
                                           "pending gate")
                                .arg(status)));
    }
}

QTEST_GUILESS_MAIN(AiHumanGateStoreTests)
#include "test_ai_human_gate_store.moc"
