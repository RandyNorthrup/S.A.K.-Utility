// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_html_email_writer.cpp
/// @brief Unit tests for HtmlEmailWriter styled HTML output

#include "sak/email_html_sanitizer.h"
#include "sak/email_types.h"
#include "sak/error_codes.h"
#include "sak/html_email_writer.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#include <tuple>

class TestHtmlEmailWriter : public QObject {
    Q_OBJECT

private Q_SLOTS:

    // ====================================================================
    // Happy Path -- Plain text message
    // ====================================================================

    void testWritePlainTextMessage() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::HtmlEmailWriter writer(temp_dir.path(), false, false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("HTML Test");
        item.sender_name = QStringLiteral("Alice");
        item.sender_email = QStringLiteral("alice@example.com");
        item.display_to = QStringLiteral("bob@example.com");
        item.body_plain = QStringLiteral("Hello, this is plain text.");
        item.date = QDateTime(QDate(2025, 6, 15), QTime(10, 30, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> no_attachments;

        auto result = writer.writeMessage(item, no_attachments, QString());
        QVERIFY(result.has_value());

        QFile file(result.value());
        QVERIFY(file.exists());
        QVERIFY(file.open(QIODevice::ReadOnly));

        QByteArray content = file.readAll();
        QVERIFY(content.contains("<!DOCTYPE html>"));
        QVERIFY(content.contains("<html lang=\"en\">"));
        QVERIFY(content.contains("<title>HTML Test</title>"));
        QVERIFY(content.contains("<h2>HTML Test</h2>"));
        // From / To / plain body are three independent emitters; a bare address substring proves
        // none of them -- it still matches with the To: row never emitted and the body wrapper
        // changed.
        QVERIFY(
            content.contains("<div class=\"field\"><span class=\"label\">From:</span> "
                             "Alice &lt;alice@example.com&gt;</div>"));
        QVERIFY(
            content.contains("<div class=\"field\"><span class=\"label\">To:</span> "
                             "bob@example.com</div>"));
        QVERIFY(content.contains("<pre>Hello, this is plain text.</pre>"));
    }

    // ====================================================================
    // HTML body preserved
    // ====================================================================

    void testHtmlBodyPreserved() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::HtmlEmailWriter writer(temp_dir.path(), false, false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Rich Content");
        item.sender_email = QStringLiteral("sender@test.com");
        item.body_html = QStringLiteral("<p>This is <b>bold</b> and <i>italic</i>.</p>");
        item.date = QDateTime(QDate(2025, 3, 1), QTime(8, 0, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> no_attachments;

        auto result = writer.writeMessage(item, no_attachments, QString());
        QVERIFY(result.has_value());

        QFile file(result.value());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QByteArray content = file.readAll();
        QVERIFY(content.contains("<p>This is <b>bold</b> and <i>italic</i>.</p>"));
        // Name-less arm of the From ternary (html_email_writer.cpp:359-361): the bare address is
        // emitted with NO angle brackets. Only the name-BEARING arm is covered elsewhere
        // (fromHeaderNotDoubleEscaped), so this arm can rot unnoticed.
        QVERIFY(
            content.contains("<div class=\"field\"><span class=\"label\">From:</span> "
                             "sender@test.com</div>"));
        QVERIFY(!content.contains("&lt;sender@test.com&gt;"));
        // The HTML branch was taken, not the plain-text branch (html_email_writer.cpp:416-420).
        QVERIFY(!content.contains("<pre>"));
    }

    // ====================================================================
    // Folder preservation
    // ====================================================================

    void testFolderPreservation() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::HtmlEmailWriter writer(temp_dir.path(), false, true);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Subfolder");
        item.sender_email = QStringLiteral("test@test.com");
        item.body_plain = QStringLiteral("Text");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> no_attachments;

        auto result = writer.writeMessage(item, no_attachments, QStringLiteral("Archive/2025"));
        QVERIFY(result.has_value());

        QString expected = temp_dir.path() + "/Archive/2025";
        QVERIFY(QDir(expected).exists());
        // The message must LAND in the preserved folder. Creating the directory and then
        // writing the page at the export root passes a dir-exists check unchanged, and this
        // also pins the no-date-prefix arm of sanitizeFilename (html_email_writer.cpp:456-458).
        QCOMPARE(result.value(), expected + QStringLiteral("/Subfolder.html"));
        QVERIFY(QFile::exists(result.value()));
    }

    // ====================================================================
    // Total bytes written
    // ====================================================================

    void testTotalBytesWritten() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::HtmlEmailWriter writer(temp_dir.path(), false, false);
        QCOMPARE(writer.totalBytesWritten(), 0);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Size Test");
        item.sender_email = QStringLiteral("test@test.com");
        item.body_plain = QStringLiteral("Content");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> no_attachments;
        const auto first = writer.writeMessage(item, no_attachments, QString());
        QVERIFY(first.has_value());

        // Exact accounting, not a floor. The counter tracks the pre-translation UTF-8 page
        // (html_email_writer.cpp:302), but the page is opened with QIODevice::Text (:292), so on
        // Windows the on-disk bytes carry CRLF -- normalize them back to LF before comparing.
        // The generated page uses \n only, so this is exact on every platform.
        QFile page1(first.value());
        QVERIFY(page1.open(QIODevice::ReadOnly));
        QByteArray disk1 = page1.readAll();
        disk1.replace("\r\n", "\n");
        const qint64 payload1 = static_cast<qint64>(disk1.size());
        QCOMPARE(writer.totalBytesWritten(), payload1);

        // A second message must ACCUMULATE ("=" instead of "+=" is invisible to one write) and
        // each saved attachment adds exactly its own byte count (html_email_writer.cpp:330).
        QVector<QPair<QString, QByteArray>> with_attachment;
        with_attachment.append({QStringLiteral("blob.bin"), QByteArray(1024, 'z')});
        const auto second = writer.writeMessage(item, with_attachment, QString());
        QVERIFY(second.has_value());
        QFile page2(second.value());
        QVERIFY(page2.open(QIODevice::ReadOnly));
        QByteArray disk2 = page2.readAll();
        disk2.replace("\r\n", "\n");
        QCOMPARE(writer.totalBytesWritten(), payload1 + static_cast<qint64>(disk2.size()) + 1024);
        std::ignore = writer.writeMessage(item, no_attachments, QString());
        QVERIFY(writer.totalBytesWritten() > 0);
    }

    // ====================================================================
    // Date prefix in filename
    // ====================================================================

    // ====================================================================
    // Attachment path traversal must be contained
    // ====================================================================

    void testAttachmentTraversalContained() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        // A canary file three levels above where a "../../../" name would land.
        const QString canary = temp_dir.path() + QStringLiteral("/canary.html");
        {
            QFile f(canary);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("ORIGINAL");
            f.close();
        }

        sak::HtmlEmailWriter writer(temp_dir.path() + QStringLiteral("/a/b/c"), false, false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Evil");
        item.sender_email = QStringLiteral("test@test.com");
        item.body_plain = QStringLiteral("body");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> attachments;
        attachments.append({QStringLiteral("../../../../canary.html"), QByteArray("PWNED")});

        auto result = writer.writeMessage(item, attachments, QString());
        QVERIFY(result.has_value());

        // The canary outside the export tree must be untouched.
        QFile c(canary);
        QVERIFY(c.open(QIODevice::ReadOnly));
        QCOMPARE(c.readAll(), QByteArray("ORIGINAL"));

        // Positive containment: the traversal name is FLATTENED to its bare basename and
        // written inside the per-message _files dir. Silently dropping the attachment while
        // still reporting success (the page keeps listing it) also leaves the canary intact,
        // so the untouched-canary check alone cannot tell containment from a fail-open drop.
        QFile saved(temp_dir.path() + QStringLiteral("/a/b/c/Evil_files/canary.html"));
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), QByteArray("PWNED"));
    }

    void testDatePrefix() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::HtmlEmailWriter writer(temp_dir.path(), true, false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Dated HTML");
        item.sender_email = QStringLiteral("test@test.com");
        item.body_plain = QStringLiteral("Content");
        item.date = QDateTime(QDate(2025, 7, 20), QTime(0, 0, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> no_attachments;
        auto result = writer.writeMessage(item, no_attachments, QString());
        QVERIFY(result.has_value());

        QFileInfo fi(result.value());
        QCOMPARE(fi.fileName(), QStringLiteral("2025-07-20_Dated HTML.html"));
    }

    // Regression: colliding subjects must never overwrite an already-written message. Order crafted
    // to trip the old counter-only scheme: "X_2" first, then two "X" (the 2nd "X" used to generate
    // "X_2" and clobber the first message's file). The fix loops until the suffixed name is free.
    void collidingSubjectsNeverClobber() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());
        sak::HtmlEmailWriter writer(temp_dir.path(),
                                    false,
                                    false);  // no date prefix, no subfolders
        const QVector<QPair<QString, QByteArray>> none;
        const auto make = [](const QString& subj, const QString& body) {
            sak::PstItemDetail item;
            item.subject = subj;
            item.body_plain = body;
            return item;
        };
        const auto r1 = writer.writeMessage(make(QStringLiteral("X_2"), QStringLiteral("BODY-ONE")),
                                            none,
                                            QString());
        const auto r2 = writer.writeMessage(make(QStringLiteral("X"), QStringLiteral("BODY-TWO")),
                                            none,
                                            QString());
        const auto r3 = writer.writeMessage(make(QStringLiteral("X"), QStringLiteral("BODY-THREE")),
                                            none,
                                            QString());
        QVERIFY(r1.has_value());
        QVERIFY(r2.has_value());
        QVERIFY(r3.has_value());
        // Three DISTINCT files, all present at once.
        // Exact dedupe shape, not just distinctness: "X_2" keeps its literal name, the first "X"
        // takes "X.html", and the second "X" must land on the first FREE slot, "X_1.html".
        QCOMPARE(r1.value(), temp_dir.path() + QStringLiteral("/X_2.html"));
        QCOMPARE(r2.value(), temp_dir.path() + QStringLiteral("/X.html"));
        QCOMPARE(r3.value(), temp_dir.path() + QStringLiteral("/X_1.html"));
        QVERIFY(QFile::exists(r1.value()));
        QVERIFY(QFile::exists(r2.value()));
        QVERIFY(QFile::exists(r3.value()));
        // Every file carries its OWN body: no message's page was overwritten or duplicated.
        QFile f1(r1.value());
        QVERIFY(f1.open(QIODevice::ReadOnly));
        QVERIFY(f1.readAll().contains("<pre>BODY-ONE</pre>"));
        QFile f2(r2.value());
        QVERIFY(f2.open(QIODevice::ReadOnly));
        QVERIFY(f2.readAll().contains("<pre>BODY-TWO</pre>"));
        QFile f3(r3.value());
        QVERIFY(f3.open(QIODevice::ReadOnly));
        QVERIFY(f3.readAll().contains("<pre>BODY-THREE</pre>"));
    }

    // Regression (B7-19): two attachments with the SAME name must both be written
    // as distinct files, not truncate/overwrite each other.
    void collidingAttachmentNamesDeduped() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());
        sak::HtmlEmailWriter writer(temp_dir.path(), false, false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("AttColl");
        item.sender_email = QStringLiteral("a@test.com");
        item.body_plain = QStringLiteral("body");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> attachments;
        attachments.append({QStringLiteral("photo.png"), QByteArray("FIRST")});
        attachments.append({QStringLiteral("photo.png"), QByteArray("SECOND")});

        auto result = writer.writeMessage(item, attachments, QString());
        QVERIFY(result.has_value());

        // Locate the per-message _files directory.
        QDir base(temp_dir.path());
        const QStringList dirs = base.entryList(QStringList{QStringLiteral("*_files")}, QDir::Dirs);
        // Named after the page, not a hash or uuid: the page's own <a href> points here.
        QCOMPARE(dirs, QStringList{QStringLiteral("AttColl_files")});
        QDir att_dir(base.filePath(dirs.first()));
        const QStringList files = att_dir.entryList(QDir::Files);
        QCOMPARE(files.size(), 2);  // both written, neither clobbered

        // Bind each payload to its EXACT deduped name: the first arrival keeps the original name
        // and the second is suffixed "_1" before the extension. Membership alone survives a swap,
        // which pairs every attachment listed on the page with the wrong file on disk.
        QFile first_file(att_dir.filePath(QStringLiteral("photo.png")));
        QVERIFY(first_file.open(QIODevice::ReadOnly));
        QCOMPARE(first_file.readAll(), QByteArray("FIRST"));
        QFile second_file(att_dir.filePath(QStringLiteral("photo_1.png")));
        QVERIFY(second_file.open(QIODevice::ReadOnly));
        QCOMPARE(second_file.readAll(), QByteArray("SECOND"));
    }

    // The From line must be escaped exactly once: the angle brackets around the
    // address render as brackets, not as literal "&lt;" text (B7-34).
    void fromHeaderNotDoubleEscaped() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());
        sak::HtmlEmailWriter writer(temp_dir.path(), false, false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("From test");
        item.sender_name = QStringLiteral("Bob");
        item.sender_email = QStringLiteral("bob@example.com");
        item.body_plain = QStringLiteral("hi");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        auto result = writer.writeMessage(item, {}, QString());
        QVERIFY(result.has_value());
        QFile file(result.value());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray content = file.readAll();

        QVERIFY(content.contains("Bob &lt;bob@example.com&gt;"));  // single escape
        QVERIFY(!content.contains("&amp;lt;"));                    // not double-escaped
    }

    // An inline image referenced by cid:<Content-ID> is embedded as a data URI; the
    // cid: reference is resolved via the attachment's Content-ID, not its filename
    // (B7-34).
    void inlineImageEmbeddedByContentId() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());
        sak::HtmlEmailWriter writer(temp_dir.path(), false, false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Inline");
        item.sender_email = QStringLiteral("a@test.com");
        item.body_html = QStringLiteral("<p><img src=\"cid:img001\"></p>");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        sak::PstAttachmentInfo att;
        att.filename = QStringLiteral("logo.png");
        att.long_filename = QStringLiteral("logo.png");
        att.content_id = QStringLiteral("img001");  // referenced by the body, not by filename
        item.attachments.append(att);

        QByteArray png = QByteArrayLiteral("\x89PNG\r\n\x1a\n");
        png.append(64, '\0');
        QVector<QPair<QString, QByteArray>> pairs;
        pairs.append({QStringLiteral("logo.png"), png});

        auto result = writer.writeMessage(item, pairs, QString());
        QVERIFY(result.has_value());
        QFile file(result.value());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray content = file.readAll();

        // Pin the WHOLE rewritten tag, payload included: a prefix-only check passes for an empty
        // or truncated base64 body, and "no cid:img001" passes when the <img> is dropped outright
        // instead of rewritten in place.
        const QByteArray expected_img =
            QByteArrayLiteral("<p><img src=\"data:image/png;base64,iVBORw0KGgoA") +
            QByteArray(84, 'A') + QByteArrayLiteral("\"></p>");
        QVERIFY(content.contains(expected_img));
        QVERIFY(!content.contains("cid:"));  // no unresolved reference of any kind
    }

    // ====================================================================
    // B7-05: untrusted email HTML must not carry active content into the
    // saved page, and the page must ship a strict CSP.
    // ====================================================================

    void sanitizerStripsActiveContent() {
        const QString dirty = QStringLiteral(
            "<p>hi</p>"
            "<script>fetch('http://evil')</script>"
            "<img src=x onerror=\"steal()\">"
            "<iframe src=\"http://evil\"></iframe>"
            "<a href=\"javascript:evil()\">go</a>");
        const QString clean = sak::sanitizeEmailBodyHtml(dirty);

        // Exact output, not "the bad substrings are gone": all four negative checks below are
        // satisfied by a sanitizer that DELETES the anchor and the image outright, and none of
        // them proves the javascript: URI was NEUTRALIZED to "blocked:" with the link text and
        // the handler-free <img> still standing.
        QCOMPARE(clean, QStringLiteral("<p>hi</p><img src=x><a href=\"blocked:evil()\">go</a>"));
        QVERIFY(clean.contains(QStringLiteral("<p>hi</p>")));  // benign markup preserved
        QVERIFY(!clean.contains(QStringLiteral("<script"), Qt::CaseInsensitive));
        QVERIFY(!clean.contains(QStringLiteral("onerror"), Qt::CaseInsensitive));
        QVERIFY(!clean.contains(QStringLiteral("<iframe"), Qt::CaseInsensitive));
        QVERIFY(!clean.contains(QStringLiteral("javascript:"), Qt::CaseInsensitive));
    }

    void savedHtmlHasCspAndStripsScript() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());
        sak::HtmlEmailWriter writer(temp_dir.path(), false, false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Hostile");
        item.sender_email = QStringLiteral("evil@test.com");
        item.body_html = QStringLiteral(
            "<p>Body</p><script>fetch('http://evil')</script>"
            "<img src=\"http://tracker/beacon.gif\" onerror=\"alert(1)\">");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        auto result = writer.writeMessage(item, {}, QString());
        QVERIFY(result.has_value());

        QFile file(result.value());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray content = file.readAll();

        // Pin the full CSP value: a weakening (http/https in img-src, dropped base-uri/
        // form-action, relaxed style-src) must fail, not slip past a header-name-only check.
        QVERIFY(
            content.contains("content=\"default-src 'none'; img-src data:; "
                             "style-src 'unsafe-inline'; font-src data:; base-uri 'none'; "
                             "form-action 'none'\""));
        QVERIFY(!content.toLower().contains("<script"));
        // The handler must be stripped FROM A SURVIVING TAG. Deleting the whole <img> also
        // satisfies a bare "no onerror" check while silently destroying legitimate images; the
        // remote src is deliberately left in place because the CSP is what blocks the fetch.
        QVERIFY(content.contains("<img src=\"http://tracker/beacon.gif\">"));
        QVERIFY(!content.toLower().contains("onerror"));
        QVERIFY(content.contains("<p>Body</p>"));  // real body still rendered
    }

    // Fail closed: if a listed attachment cannot be saved, writeMessage must NOT
    // emit an HTML page that advertises a file not on disk. Here the per-message
    // "_files" directory cannot be created because a regular file already occupies
    // that exact path, so the attachment save fails and the whole write is refused.
    void attachmentSaveFailureFailsClosed() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());
        sak::HtmlEmailWriter writer(temp_dir.path(), false, false);

        // The _files dir is "<base>_files" where <base> is the sanitized subject.
        const QString blocker = temp_dir.path() + QStringLiteral("/AttFail_files");
        {
            QFile f(blocker);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("BLOCK");
            f.close();
        }

        sak::PstItemDetail item;
        item.subject = QStringLiteral("AttFail");
        item.sender_email = QStringLiteral("a@test.com");
        item.body_plain = QStringLiteral("body");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> attachments;
        attachments.append({QStringLiteral("doc.pdf"), QByteArray("DATA")});

        const auto result = writer.writeMessage(item, attachments, QString());
        QVERIFY(!result.has_value());  // fail closed, not a partial success
        QCOMPARE(result.error(), sak::error_code::write_error);

        // No HTML page was left behind claiming a complete message.
        QVERIFY(!QFile::exists(temp_dir.path() + QStringLiteral("/AttFail.html")));

        // write_error has THREE producers (unusable output root, attachment save failure, page
        // write failure), so the code alone does not prove which guard fired. Second,
        // directory-independent failure: a NULL payload -- content that was never delivered --
        // is refused inside the per-attachment loop (email_attachment_saver.cpp:92-99), so a
        // pre-flight "_files path is occupied" check could not stand in for it.
        QTemporaryDir null_root;
        QVERIFY(null_root.isValid());
        sak::HtmlEmailWriter null_writer(null_root.path(), false, false);
        QVector<QPair<QString, QByteArray>> null_attachment;
        null_attachment.append({QStringLiteral("doc.pdf"), QByteArray()});
        const auto null_result = null_writer.writeMessage(item, null_attachment, QString());
        QVERIFY(!null_result.has_value());
        QCOMPARE(null_result.error(), sak::error_code::write_error);
        QVERIFY(!QFile::exists(null_root.path() + QStringLiteral("/AttFail.html")));

        // Control arm: the same item and attachment against a clean root succeed, so the
        // refusals above are caused by the failing attachment and not by the fixture.
        QTemporaryDir clean_root;
        QVERIFY(clean_root.isValid());
        sak::HtmlEmailWriter ok_writer(clean_root.path(), false, false);
        const auto ok = ok_writer.writeMessage(item, attachments, QString());
        QVERIFY(ok.has_value());
        QCOMPARE(ok.value(), clean_root.path() + QStringLiteral("/AttFail.html"));
        QFile saved(clean_root.path() + QStringLiteral("/AttFail_files/doc.pdf"));
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), QByteArray("DATA"));

        // No HTML page was left behind claiming a complete message.
        QVERIFY(!QFile::exists(temp_dir.path() + QStringLiteral("/AttFail.html")));
    }

    // A subfolder path escaping the output directory must be refused (defense-in-
    // depth), not silently written outside the export tree.
    void rejectsSubfolderTraversal() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());
        // Root the export tree one level DOWN so the "../" target is a path this test owns (with
        // the writer rooted at the QTemporaryDir itself, the escape target is the HOST system
        // temp directory).
        const QString root = temp_dir.path() + QStringLiteral("/root");
        QVERIFY(QDir().mkpath(root));
        sak::HtmlEmailWriter writer(root, false, true);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("x");
        item.sender_email = QStringLiteral("s@test.com");
        item.body_plain = QStringLiteral("b");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        auto result = writer.writeMessage(item, {}, QStringLiteral("../escape"));
        QVERIFY(!result.has_value());
        QCOMPARE(result.error(), sak::error_code::path_traversal_attempt);
        // Refused BEFORE anything exists outside the root. path_traversal_attempt has two
        // producers -- a lexical check and a junction check -- and the second only runs AFTER
        // mkpath, so without this the lexical guard can be deleted entirely and the test still
        // passes, with an attacker-named directory planted outside the export tree.
        QVERIFY(!QDir(temp_dir.path() + QStringLiteral("/escape")).exists());
        // Accept arm of the same guard: a non-escaping subfolder is written under the root.
        const auto ok = writer.writeMessage(item, {}, QStringLiteral("sub"));
        QVERIFY(ok.has_value());
        QCOMPARE(ok.value(), root + QStringLiteral("/sub/x.html"));
    }
};

QTEST_MAIN(TestHtmlEmailWriter)

#include "test_html_email_writer.moc"
