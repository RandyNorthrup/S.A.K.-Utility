// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_imap_uploader.cpp
/// @brief Security-seam tests for the IMAP uploader (B9-01..05): flag-token
///        validation, line-prefix response parsing, the parallel-vector guard,
///        and plaintext refusal. The network session itself is not exercised.

#include "sak/error_codes.h"
#include "sak/imap_uploader.h"
#include "sak/ost_converter_types.h"

#include <QtTest/QtTest>

using sak::ImapUploader;

class ImapUploaderTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // B9-03: flag-token validation.
    void isValidImapFlag_acceptsAtomsAndSystemFlags() {
        QVERIFY(ImapUploader::isValidImapFlag(QStringLiteral("\\Seen")));
        QVERIFY(ImapUploader::isValidImapFlag(QStringLiteral("\\Flagged")));
        QVERIFY(ImapUploader::isValidImapFlag(QStringLiteral("Keyword")));
        QVERIFY(ImapUploader::isValidImapFlag(QStringLiteral("$Label1")));
    }

    void isValidImapFlag_rejectsInjectionAndMalformed() {
        // A flag that could break out of the "(...)" list or inject a command.
        QVERIFY(!ImapUploader::isValidImapFlag(QString()));
        QVERIFY(!ImapUploader::isValidImapFlag(QStringLiteral("\\")));   // bare backslash
        QVERIFY(!ImapUploader::isValidImapFlag(QStringLiteral("\\Seen)")));
        QVERIFY(!ImapUploader::isValidImapFlag(QStringLiteral("a b")));  // space
        QVERIFY(!ImapUploader::isValidImapFlag(QStringLiteral("a\r\nA1 LOGOUT")));
        QVERIFY(!ImapUploader::isValidImapFlag(QStringLiteral("(x)")));
        QVERIFY(!ImapUploader::isValidImapFlag(QStringLiteral("a\"b")));
        QVERIFY(!ImapUploader::isValidImapFlag(QStringLiteral("a{3}")));
    }

    // B9-05: tagged/continuation status must be read only at a line boundary.
    void taggedLineIsOk_onlyMatchesAtLineStart() {
        // A real tagged completion line.
        QVERIFY(ImapUploader::taggedLineIsOk(QStringLiteral("A0007 OK APPEND completed\r\n"),
                                             QStringLiteral("A0007")));
        // Untagged data preceding the tagged line still resolves the tagged line.
        QVERIFY(ImapUploader::taggedLineIsOk(QStringLiteral("* 1 EXISTS\r\nA0007 OK done\r\n"),
                                             QStringLiteral("A0007")));
        // A forged "A0007 OK" buried inside a message body / untagged line must NOT
        // count -- it is not at a line start.
        QVERIFY(!ImapUploader::taggedLineIsOk(
            QStringLiteral("* OK body says A0007 OK haha\r\nA0007 NO rejected\r\n"),
            QStringLiteral("A0007")));
        // An incomplete line (no CRLF yet) is not decided early.
        QVERIFY(!ImapUploader::taggedLineIsOk(QStringLiteral("A0007 OK no newline yet"),
                                              QStringLiteral("A0007")));
    }

    // Greeting must be an untagged success greeting; a BYE/NO that merely contains
    // "OK" somewhere must be rejected before any credential is sent.
    void isValidImapGreeting_onlyAcceptsOkOrPreauth() {
        QVERIFY(
            ImapUploader::isValidImapGreeting(QStringLiteral("* OK IMAP4rev1 Service Ready\r\n")));
        QVERIFY(ImapUploader::isValidImapGreeting(
            QStringLiteral("* PREAUTH already authenticated\r\n")));
        // "* BYE" that happens to embed the substring "OK" must NOT pass.
        QVERIFY(!ImapUploader::isValidImapGreeting(
            QStringLiteral("* BYE server LOOKS busy, try later\r\n")));
        QVERIFY(!ImapUploader::isValidImapGreeting(QStringLiteral("* NO not available\r\n")));
        // A partial read with no complete line yet is not decided early.
        QVERIFY(!ImapUploader::isValidImapGreeting(QStringLiteral("* OK still coming")));
    }

    void hasCompleteLineWithPrefix_continuationOnlyAtLineStart() {
        // A continuation request is a line that starts with '+'.
        QVERIFY(ImapUploader::hasCompleteLineWithPrefix(QStringLiteral("+ Ready\r\n"),
                                                        QStringLiteral("+")));
        // A '+' inside an untagged line must not be read as a continuation.
        QVERIFY(!ImapUploader::hasCompleteLineWithPrefix(QStringLiteral("* 1+1 note\r\n"),
                                                         QStringLiteral("+")));
    }

    // B9-01: mismatched parallel vectors must fail closed, not drive an OOB at().
    void uploadFolder_rejectsMismatchedVectorLengths() {
        ImapUploader uploader;
        sak::ImapServerConfig config;
        config.host = QStringLiteral("imap.invalid");
        config.use_ssl = true;
        const QVector<QByteArray> messages{QByteArrayLiteral("a"), QByteArrayLiteral("b")};
        const QVector<QStringList> flags{{QStringLiteral("\\Seen")}};  // shorter on purpose
        const QVector<QDateTime> dates{QDateTime::currentDateTime(), QDateTime::currentDateTime()};

        const auto result =
            uploader.uploadFolder(config, QStringLiteral("Inbox"), messages, flags, dates);
        QVERIFY(!result.has_value());
        QCOMPARE(result.error(), sak::error_code::invalid_argument);
    }

    // B9-04: a plaintext (use_ssl=false) session must be refused before any
    // credentials are sent -- the worker rejects it at start, no socket connects.
    void testConnection_refusesPlaintext() {
        ImapUploader uploader;
        sak::ImapServerConfig config;
        config.host = QStringLiteral("imap.invalid");
        config.use_ssl = false;
        const auto result = uploader.testConnection(config);
        QVERIFY(!result.has_value());
        QCOMPARE(result.error(), sak::error_code::connection_failed);
    }
};

QTEST_GUILESS_MAIN(ImapUploaderTests)
#include "test_imap_uploader.moc"
