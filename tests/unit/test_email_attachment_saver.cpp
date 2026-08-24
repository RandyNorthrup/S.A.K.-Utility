// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_email_attachment_saver.cpp
/// @brief Unit tests for the shared attachment-saving utilities

#include "sak/email_attachment_saver.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class TestEmailAttachmentSaver : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void sanitizeStripsInvalidChars();
    void sanitizeStripsPathSeparators();  // R5-G10-9
    void saveToPathWritesExactBytes();
    void saveToDirectoryDeduplicates();
    void saveToDirectoryDoesNotTruncateWhenExhausted();
    void batchRefusesArrivalItDidNotRequest();
    void batchRecordsEachExpectedRefOnce();
    void batchRecordErrorCountsOnlyOutstandingRef();
    void batchRecordErrorRefusesDuplicate();
    void batchRefusesOverlappingBegin();
    void batchRefusesEmptyExpectation();
};

void TestEmailAttachmentSaver::sanitizeStripsInvalidChars() {
    QCOMPARE(sak::sanitizeAttachmentFilename(QStringLiteral("a<b>c:d.txt")),
             QStringLiteral("a_b_c_d.txt"));
    QCOMPARE(sak::sanitizeAttachmentFilename(QString()), QStringLiteral("attachment"));
    QCOMPARE(sak::sanitizeAttachmentFilename(QStringLiteral("name...")), QStringLiteral("name"));
    // The rest of the Windows invalid-character catalog: '"', '|', '?' and '*' are
    // stripped too, not just the three exercised above (and '/' '\\' in
    // sanitizeStripsPathSeparators).
    QCOMPARE(sak::sanitizeAttachmentFilename(QStringLiteral("q\"u|e?s*t.txt")),
             QStringLiteral("q_u_e_s_t.txt"));
    // A name that passes the empty check but sanitizes down to nothing falls back to
    // "attachment" as well -- a SECOND, distinct fallback site from the empty-input one.
    QCOMPARE(sak::sanitizeAttachmentFilename(QStringLiteral("...")), QStringLiteral("attachment"));
    QCOMPARE(sak::sanitizeAttachmentFilename(QStringLiteral("   ")), QStringLiteral("attachment"));
}

void TestEmailAttachmentSaver::sanitizeStripsPathSeparators() {
    // An attacker-authored attachment filename with path separators / traversal must not be
    // able to escape the target directory when saved: both '/' and '\' are replaced with '_'
    // so the sanitized name is a single path COMPONENT that QDir::filePath cannot walk out of.
    QCOMPARE(sak::sanitizeAttachmentFilename(QStringLiteral("../../secret.txt")),
             QStringLiteral(".._.._secret.txt"));
    QCOMPARE(sak::sanitizeAttachmentFilename(QStringLiteral("..\\..\\Windows\\System32\\evil.dll")),
             QStringLiteral(".._.._Windows_System32_evil.dll"));

    // Whatever the input, the result carries no separator, so it cannot traverse.
    const QString mixed = sak::sanitizeAttachmentFilename(QStringLiteral("a/b\\c"));
    QCOMPARE(mixed, QStringLiteral("a_b_c"));  // both separators -> '_', exact contract output

    // Non-vacuity: an ordinary filename with no separators is preserved unchanged, so the
    // replacement targets separators specifically rather than mangling every name.
    QCOMPARE(sak::sanitizeAttachmentFilename(QStringLiteral("report.pdf")),
             QStringLiteral("report.pdf"));
}

void TestEmailAttachmentSaver::saveToPathWritesExactBytes() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + QStringLiteral("/file.bin");
    const QByteArray data("hello world payload", 19);

    const auto result = sak::saveAttachmentToPath(path, data);
    QVERIFY(result.success);
    QCOMPARE(result.saved_path, path);

    QFile written(path);
    QVERIFY(written.open(QIODevice::ReadOnly));
    QCOMPARE(written.readAll(), data);
}

void TestEmailAttachmentSaver::saveToDirectoryDeduplicates() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray original("ORIGINAL", 8);
    const QByteArray second("SECOND", 6);

    const auto first =
        sak::saveAttachmentToDirectory(dir.path(), QStringLiteral("a.txt"), original);
    QVERIFY(first.success);
    QCOMPARE(first.saved_path, dir.path() + QStringLiteral("/a.txt"));

    // A second save of the same name must NOT overwrite the first.
    const auto next = sak::saveAttachmentToDirectory(dir.path(), QStringLiteral("a.txt"), second);
    QVERIFY(next.success);
    QCOMPARE(next.saved_path, dir.path() + QStringLiteral("/a_1.txt"));

    QFile untouched(first.saved_path);
    QVERIFY(untouched.open(QIODevice::ReadOnly));
    QCOMPARE(untouched.readAll(), original);

    // ...and the deduped slot holds the SECOND payload, not a re-write of the first.
    QFile deduped(next.saved_path);
    QVERIFY(deduped.open(QIODevice::ReadOnly));
    QCOMPARE(deduped.readAll(), second);

    // The counter is inserted before the extension only when there IS one (dot > 0).
    // "a.txt" only ever exercises that arm; a name with no dot and a dot-LEADING name
    // both take the suffix at the end.
    const QByteArray third("THIRD", 5);
    const QString noext = QStringLiteral("readme");
    const auto noext_a = sak::saveAttachmentToDirectory(dir.path(), noext, original);
    const auto noext_b = sak::saveAttachmentToDirectory(dir.path(), noext, third);
    QVERIFY(noext_a.success);
    QVERIFY(noext_b.success);
    QCOMPARE(noext_a.saved_path, dir.path() + QStringLiteral("/readme"));
    QCOMPARE(noext_b.saved_path, dir.path() + QStringLiteral("/readme_1"));

    const QString dotfile = QStringLiteral(".hidden");
    const auto dot_a = sak::saveAttachmentToDirectory(dir.path(), dotfile, original);
    const auto dot_b = sak::saveAttachmentToDirectory(dir.path(), dotfile, third);
    QVERIFY(dot_a.success);
    QVERIFY(dot_b.success);
    QCOMPARE(dot_a.saved_path, dir.path() + QStringLiteral("/.hidden"));
    QCOMPARE(dot_b.saved_path, dir.path() + QStringLiteral("/.hidden_1"));
}

void TestEmailAttachmentSaver::saveToDirectoryDoesNotTruncateWhenExhausted() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // Occupy the base name and every dedupe slot _1.._999.
    const QByteArray sentinel("KEEP", 4);
    const auto writeFile = [](const QString& p, const QByteArray& b) {
        QFile f(p);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(b);
    };
    writeFile(dir.path() + QStringLiteral("/a.txt"), sentinel);
    for (int i = 1; i <= 999; ++i) {
        writeFile(dir.path() + QStringLiteral("/a_%1.txt").arg(i), sentinel);
    }

    const QByteArray attacker("OVERWRITE", 9);
    const auto result =
        sak::saveAttachmentToDirectory(dir.path(), QStringLiteral("a.txt"), attacker);
    QVERIFY(!result.success);
    // Refused for the STATED reason (name exhaustion), not by the empty-dir or
    // null-payload guard, and only after walking every dedupe slot up to _999.
    QCOMPARE(result.error_message,
             QStringLiteral("No unique attachment name available in: ") + dir.path());
    QCOMPARE(result.saved_path, dir.path() + QStringLiteral("/a_999.txt"));

    // The last slot must be intact, not truncated to the new payload.
    QFile last(dir.path() + QStringLiteral("/a_999.txt"));
    QVERIFY(last.open(QIODevice::ReadOnly));
    QCOMPARE(last.readAll(), sentinel);
}

// A batch must only ever record the attachments it asked for. Attachment content
// arrives asynchronously and is addressed by (message id, attachment index); a
// delivery that names anything else belongs to some other request -- an inline
// image fetch, or a leftover from an earlier batch -- and writing it would put the
// wrong payload in one of this batch's slots while the batch still counted the
// slot as filled.
void TestEmailAttachmentSaver::batchRefusesArrivalItDidNotRequest() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    sak::AttachmentBatchSave batch;
    const sak::AttachmentRef wanted{42, 0};
    QVERIFY(batch.begin(dir.path(), {wanted}));

    // A different message, and a different attachment of the same message.
    const sak::AttachmentRef other_message{43, 0};
    const sak::AttachmentRef other_index{42, 1};
    QVERIFY(!batch.expects(other_message));
    QVERIFY(!batch.expects(other_index));

    const auto stray =
        batch.recordOne(other_message, QStringLiteral("report.pdf"), QByteArray("STRAY", 5));
    QVERIFY(!stray.success);
    QCOMPARE(batch.succeeded(), 0);
    QCOMPARE(batch.failed(), 0);
    QVERIFY(!batch.isComplete());
    QVERIFY(!QFile::exists(dir.path() + QStringLiteral("/report.pdf")));

    // The arrival the batch did ask for still fills the slot, with its own bytes.
    QVERIFY(batch.expects(wanted));
    const auto real = batch.recordOne(wanted, QStringLiteral("report.pdf"), QByteArray("REAL", 4));
    QVERIFY(real.success);
    QCOMPARE(batch.succeeded(), 1);
    QVERIFY(batch.isComplete());

    QFile written(dir.path() + QStringLiteral("/report.pdf"));
    QVERIFY(written.open(QIODevice::ReadOnly));
    QCOMPARE(written.readAll(), QByteArray("REAL", 4));
}

void TestEmailAttachmentSaver::batchRecordsEachExpectedRefOnce() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    sak::AttachmentBatchSave batch;
    const sak::AttachmentRef first{7, 0};
    const sak::AttachmentRef second{7, 1};
    QVERIFY(batch.begin(dir.path(), {first, second}));
    QCOMPARE(batch.expectedCount(), 2);

    QVERIFY(batch.recordOne(first, QStringLiteral("a.txt"), QByteArray("ONE", 3)).success);

    // A duplicate delivery of an already-recorded ref is no longer outstanding: it
    // must not be saved as a deduped second copy, and must not count as progress.
    QVERIFY(!batch.expects(first));
    QVERIFY(!batch.recordOne(first, QStringLiteral("a.txt"), QByteArray("DUP", 3)).success);
    QVERIFY(!batch.isComplete());
    QVERIFY(!QFile::exists(dir.path() + QStringLiteral("/a_1.txt")));

    QVERIFY(batch.recordOne(second, QStringLiteral("b.txt"), QByteArray("TWO", 3)).success);
    QVERIFY(batch.isComplete());
    QCOMPARE(batch.succeeded(), 2);
    QCOMPARE(batch.failed(), 0);
    // The counters reach the user only through summaryText(); its clean arm must name
    // the SAVED count and the directory the bytes actually went to.
    QCOMPARE(batch.summaryText(), QStringLiteral("Saved 2 attachment(s) to ") + dir.path());
}

// G22-9: a batch save must count a FAILURE by attachment identity, not off a bare
// "something errored" signal. A failure naming an attachment this batch is not waiting
// on -- an unrelated controller error, or a request refused for a different operation --
// must never be charged to the batch, because that would inflate the failed count and
// could complete the batch before its own requests resolved.
void TestEmailAttachmentSaver::batchRecordErrorCountsOnlyOutstandingRef() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    sak::AttachmentBatchSave batch;
    const sak::AttachmentRef first{7, 0};
    const sak::AttachmentRef second{7, 1};
    QVERIFY(batch.begin(dir.path(), {first, second}));

    // A failure for an attachment this batch never asked for is ignored: nothing
    // counted, batch not advanced toward completion.
    QVERIFY(!batch.recordError(sak::AttachmentRef{999, 0}));
    QCOMPARE(batch.failed(), 0);
    QVERIFY(!batch.isComplete());

    // A failure for an outstanding attachment counts once and clears the slot.
    QVERIFY(batch.recordError(first));
    QCOMPARE(batch.failed(), 1);
    QVERIFY(!batch.expects(first));
    QVERIFY(!batch.isComplete());

    // The batch completes only once its real requests have all resolved.
    QVERIFY(batch.recordOne(second, QStringLiteral("b.txt"), QByteArray("TWO", 3)).success);
    QVERIFY(batch.isComplete());
    QCOMPARE(batch.succeeded(), 1);
    QCOMPARE(batch.failed(), 1);
    QCOMPARE(batch.expectedCount(), 2);
}

// A second failure for an attachment that is already resolved -- whether it succeeded
// or failed -- is no longer outstanding and must not double-count.
void TestEmailAttachmentSaver::batchRecordErrorRefusesDuplicate() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    sak::AttachmentBatchSave batch;
    const sak::AttachmentRef only{5, 2};
    QVERIFY(batch.begin(dir.path(), {only}));

    QVERIFY(batch.recordError(only));
    QCOMPARE(batch.failed(), 1);
    QVERIFY(batch.isComplete());
    // The failure arm of the same user-facing summary: saved / expected / failed, each
    // count in its own slot.
    QCOMPARE(batch.summaryText(), QStringLiteral("Saved 0 of 1 attachment(s) (1 failed)"));

    // A duplicate failure for the same ref is refused: no second increment.
    QVERIFY(!batch.recordError(only));
    QCOMPARE(batch.failed(), 1);

    // A late success for the same ref is likewise refused (not outstanding), so the
    // counts stay honest.
    QVERIFY(!batch.recordOne(only, QStringLiteral("late.txt"), QByteArray("LATE", 4)).success);
    QCOMPARE(batch.succeeded(), 0);
}

void TestEmailAttachmentSaver::batchRefusesOverlappingBegin() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    sak::AttachmentBatchSave batch;
    QVERIFY(batch.begin(dir.path(), {sak::AttachmentRef{1, 0}, sak::AttachmentRef{1, 1}}));
    const auto first_saved =
        batch.recordOne(sak::AttachmentRef{1, 0}, QStringLiteral("a.txt"), QByteArray("ONE", 3));
    QVERIFY(first_saved.success);

    // Two live expectation sets would interleave their arrivals, so the second batch is
    // refused and the first one is left FULLY untouched: same target directory (the
    // refused begin names a DIFFERENT one, so a clobbered m_dir is visible), same
    // expectation set, same progress counters.
    QTemporaryDir other_dir;
    QVERIFY(other_dir.isValid());
    QVERIFY(!batch.begin(other_dir.path(), {sak::AttachmentRef{2, 0}}));
    QCOMPARE(batch.directory(), dir.path());
    QCOMPARE(batch.expectedCount(), 2);
    QCOMPARE(batch.succeeded(), 1);
    QCOMPARE(batch.failed(), 0);
    QVERIFY(batch.expects(sak::AttachmentRef{1, 1}));
    QVERIFY(!batch.expects(sak::AttachmentRef{2, 0}));

    // The still-outstanding arrival lands in the ORIGINAL directory, not the one the
    // refused begin() named.
    const auto second_saved =
        batch.recordOne(sak::AttachmentRef{1, 1}, QStringLiteral("b.txt"), QByteArray("TWO", 3));
    QVERIFY(second_saved.success);
    QCOMPARE(second_saved.saved_path, dir.path() + QStringLiteral("/b.txt"));
    QVERIFY(QFile::exists(dir.path() + QStringLiteral("/b.txt")));
    QVERIFY(!QFile::exists(other_dir.path() + QStringLiteral("/b.txt")));

    batch.reset();
    QVERIFY(!batch.isActive());
    QVERIFY(batch.begin(dir.path(), {sak::AttachmentRef{2, 0}}));
}

void TestEmailAttachmentSaver::batchRefusesEmptyExpectation() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    sak::AttachmentBatchSave batch;
    QVERIFY(!batch.begin(dir.path(), {}));
    QVERIFY(!batch.isActive());
    QVERIFY(!batch.begin(QString(), {sak::AttachmentRef{1, 0}}));
    QVERIFY(!batch.isActive());

    // No batch started, so nothing can be recorded into one.
    QVERIFY(!batch.expects(sak::AttachmentRef{1, 0}));
    QVERIFY(!batch.recordOne(sak::AttachmentRef{1, 0}, QStringLiteral("x.txt"), QByteArray("X", 1))
                 .success);
    QVERIFY(!QFile::exists(dir.path() + QStringLiteral("/x.txt")));
}

QTEST_MAIN(TestEmailAttachmentSaver)
#include "test_email_attachment_saver.moc"
