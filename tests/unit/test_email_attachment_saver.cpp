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
    void saveToPathWritesExactBytes();
    void saveToDirectoryDeduplicates();
    void saveToDirectoryDoesNotTruncateWhenExhausted();
};

void TestEmailAttachmentSaver::sanitizeStripsInvalidChars() {
    QCOMPARE(sak::sanitizeAttachmentFilename(QStringLiteral("a<b>c:d.txt")),
             QStringLiteral("a_b_c_d.txt"));
    QCOMPARE(sak::sanitizeAttachmentFilename(QString()), QStringLiteral("attachment"));
    QCOMPARE(sak::sanitizeAttachmentFilename(QStringLiteral("name...")), QStringLiteral("name"));
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
    QVERIFY(next.saved_path != first.saved_path);

    QFile untouched(first.saved_path);
    QVERIFY(untouched.open(QIODevice::ReadOnly));
    QCOMPARE(untouched.readAll(), original);
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

    // The last slot must be intact, not truncated to the new payload.
    QFile last(dir.path() + QStringLiteral("/a_999.txt"));
    QVERIFY(last.open(QIODevice::ReadOnly));
    QCOMPARE(last.readAll(), sentinel);
}

QTEST_MAIN(TestEmailAttachmentSaver)
#include "test_email_attachment_saver.moc"
