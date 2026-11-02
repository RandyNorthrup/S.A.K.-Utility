// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_pst_store_structure.cpp
/// @brief Whole generated stores carrying the shapes real Outlook files were found to have.
///
/// Each case here reproduces, in invented data, a structure that a real mailbox exposed and the
/// tiny single-purpose fixtures in pst_fixture.h could not.

#include "sak/email_folder_selection.h"
#include "sak/email_types.h"
#include "sak/error_codes.h"
#include "sak/pst_named_properties.h"
#include "sak/pst_parser.h"

#include "../support/pst_store_builder.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using namespace sak::pst_fixture;

namespace {

constexpr uint8_t kSetAppointment = 0x02;
constexpr uint8_t kSetTask = 0x03;
constexpr uint8_t kSetAddress = 0x04;
constexpr uint8_t kSetNote = 0x0E;

BuilderMessage plainMessage(const QString& subject) {
    BuilderMessage msg;
    msg.subject = subject;
    msg.sender_name = QStringLiteral("Marisol Quint");
    msg.sender_email = QStringLiteral("marisol@harborlane.example");
    msg.display_to = QStringLiteral("Dana Whitlock");
    msg.date = QDateTime(QDate(2026, 3, 9), QTime(14, 5), QTimeZone::UTC);
    msg.body_plain = QStringLiteral("Body of ") + subject;
    return msg;
}

/// A literal-only LZFu stream, the simplest valid compressed RTF.
QByteArray compressRtfLiterally(const QByteArray& rtf) {
    constexpr int kPrebuiltDictionaryBytes = 207;
    constexpr int kDictionaryMask = 0x0FFF;
    constexpr int kControlBits = 8;
    const int end_position = (kPrebuiltDictionaryBytes + static_cast<int>(rtf.size())) &
                             kDictionaryMask;
    const uint16_t end_reference = static_cast<uint16_t>(end_position << 4);

    QByteArray stream;
    qsizetype pos = 0;
    bool ended = false;
    while (!ended) {
        uint8_t control = 0;
        QByteArray group;
        for (int bit = 0; bit < kControlBits && !ended; ++bit) {
            if (pos < rtf.size()) {
                group.append(rtf.at(pos++));
            } else {
                // The end marker is a reference to the write position, flagged by a set bit.
                control = static_cast<uint8_t>(control | (1U << bit));
                group.append(static_cast<char>(end_reference >> 8));
                group.append(static_cast<char>(end_reference & 0xFF));
                ended = true;
            }
        }
        stream.append(static_cast<char>(control));
        stream.append(group);
    }
    QByteArray header(16, '\0');
    writeLe32(header, 0, static_cast<uint32_t>(stream.size() + 12));
    writeLe32(header, 4, static_cast<uint32_t>(rtf.size()));
    header.replace(8, 4, "LZFu");
    writeLe32(header, 12, weakCrc(stream, 0, static_cast<int>(stream.size())));
    return header + stream;
}

}  // namespace

class TestPstStoreStructure : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    int m_file_counter = 0;

    [[nodiscard]] QString writeStore(const QByteArray& bytes) {
        const QString path = m_dir.filePath(QStringLiteral("store%1.pst").arg(++m_file_counter));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
            return {};
        }
        return path;
    }

    static const sak::PstFolder* folderNamed(const sak::PstFolderTree& tree, const QString& name) {
        for (const auto& folder : tree) {
            if (folder.display_name == name) {
                return &folder;
            }
            if (const auto* found = folderNamed(folder.children, name)) {
                return found;
            }
        }
        return nullptr;
    }

    /// The single message in the folder called @p folder_name.
    static std::expected<sak::PstItemDetail, sak::error_code> onlyMessage(
        PstParser& parser, const QString& folder_name) {
        const auto* folder = folderNamed(parser.folderTree(), folder_name);
        if (folder == nullptr) {
            return std::unexpected(sak::error_code::not_found);
        }
        const auto items = parser.readFolderItems(folder->node_id, 0, 0);
        if (!items || items->size() != 1) {
            return std::unexpected(sak::error_code::not_found);
        }
        return parser.readItemDetail(items->first().node_id);
    }

private Q_SLOTS:
    void initTestCase() { QVERIFY(m_dir.isValid()); }

    void buildsAStoreLargeEnoughToNeedIndexPages() {
        BuilderFolder inbox;
        inbox.name = QStringLiteral("Inbox");
        for (int i = 0; i < 40; ++i) {
            inbox.messages.append(plainMessage(QStringLiteral("Message %1").arg(i)));
        }
        PstParser parser;
        parser.open(writeStore(PstStoreBuilder{}.build({inbox})));
        QVERIFY(parser.isOpen());
        const auto* folder = folderNamed(parser.folderTree(), QStringLiteral("Inbox"));
        QVERIFY(folder != nullptr);
        const auto items = parser.readFolderItems(folder->node_id, 0, 0);
        QVERIFY(items.has_value());
        QCOMPARE(items->size(), 40);
        QCOMPARE(items->at(7).subject, QStringLiteral("Message 7"));
        QCOMPARE(items->at(7).sender_email, QStringLiteral("marisol@harborlane.example"));
    }

    // Real stores set MS-PST's reserved BID bit on some references. An exact-match lookup hid the
    // two largest folders of a real 1.1 GB PST behind "Block not found".
    void readsABlockReferencedWithTheReservedBitSet() {
        BuilderMessage msg = plainMessage(QStringLiteral("Reserved bit"));
        msg.data_bid_reserved_bit = true;
        BuilderFolder inbox;
        inbox.name = QStringLiteral("Inbox");
        inbox.messages.append(msg);
        PstParser parser;
        parser.open(writeStore(PstStoreBuilder{}.build({inbox})));
        QVERIFY(parser.isOpen());
        const auto detail = onlyMessage(parser, QStringLiteral("Inbox"));
        QVERIFY(detail.has_value());
        QCOMPARE(detail->subject, QStringLiteral("Reserved bit"));
    }

    void readsABlockWhoseBbtKeyCarriesTheReservedBit() {
        BuilderFolder inbox;
        inbox.name = QStringLiteral("Inbox");
        inbox.messages.append(plainMessage(QStringLiteral("Reserved key")));
        PstStoreBuilder builder;
        builder.bbt_keys_reserved_bit = true;
        PstParser parser;
        parser.open(writeStore(builder.build({inbox})));
        QVERIFY(parser.isOpen());
        const auto detail = onlyMessage(parser, QStringLiteral("Inbox"));
        QVERIFY(detail.has_value());
        QCOMPARE(detail->subject, QStringLiteral("Reserved key"));
    }

    void findsTheIpmSubtreeByTheStoresPointerWhateverItsName() {
        BuilderFolder inbox;
        inbox.name = QStringLiteral("Inbox");
        PstStoreBuilder builder;
        builder.ipm_subtree_name = QStringLiteral("Oberste Ebene der Outlook-Datendatei");
        PstParser parser;
        parser.open(writeStore(builder.build({inbox})));
        QVERIFY(parser.isOpen());
        const auto* subtree = sak::findIpmSubtree(parser.folderTree());
        QVERIFY(subtree != nullptr);
        QCOMPARE(subtree->display_name, builder.ipm_subtree_name);
        // And "all mail folders" therefore starts below it, not at the unnamed root.
        const auto* inbox_folder = folderNamed(parser.folderTree(), QStringLiteral("Inbox"));
        QVERIFY(inbox_folder != nullptr);
        QCOMPARE(sak::mailFolderNodeIds(parser.folderTree()),
                 QVector<uint64_t>{inbox_folder->node_id});
    }

    // A store's declared count can exceed its rows; the list must not promise pages of nothing.
    void reportsTheRowsAFolderHoldsNotItsDeclaredCount() {
        BuilderFolder inbox;
        inbox.name = QStringLiteral("Inbox");
        inbox.declared_count = 300;
        inbox.messages.append(plainMessage(QStringLiteral("One")));
        inbox.messages.append(plainMessage(QStringLiteral("Two")));
        PstParser parser;
        parser.open(writeStore(PstStoreBuilder{}.build({inbox})));
        const auto* folder = folderNamed(parser.folderTree(), QStringLiteral("Inbox"));
        QVERIFY(folder != nullptr);
        QCOMPARE(folder->content_count, 300);

        QSignalSpy spy(&parser, &PstParser::folderItemsLoaded);
        parser.loadFolderItems(folder->node_id, 0, 100);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(2).toInt(), 2);
    }

    void readsCalendarFieldsFromNamedProperties() {
        BuilderMessage event;
        event.message_class = QStringLiteral("IPM.Appointment");
        event.subject = QStringLiteral("Health inspection");
        const QDateTime start(QDate(2026, 4, 2), QTime(9, 0), QTimeZone::UTC);
        const QDateTime end(QDate(2026, 4, 2), QTime(10, 30), QTimeZone::UTC);
        event.named_props = {
            {kSetAppointment,
             sak::pst::kLidAppointmentStartWhole,
             sak::email::kPropTypeSysTime,
             builderFiletime(start)},
            {kSetAppointment,
             sak::pst::kLidAppointmentEndWhole,
             sak::email::kPropTypeSysTime,
             builderFiletime(end)},
            {kSetAppointment,
             sak::pst::kLidAppointmentLocation,
             sak::email::kPropTypeUnicode,
             builderUnicode(QStringLiteral("Front kitchen"))},
            {kSetAppointment,
             sak::pst::kLidAppointmentBusyStatus,
             sak::email::kPropTypeInt32,
             builderInt32(2)},
            {kSetAppointment,
             sak::pst::kLidAppointmentSubType,
             sak::email::kPropTypeBoolean,
             QByteArray(1, 1)},
            // A property id with the same long id in another set must not be read as this one.
            {kSetTask,
             sak::pst::kLidAppointmentLocation,
             sak::email::kPropTypeUnicode,
             builderUnicode(QStringLiteral("Wrong set"))},
        };
        BuilderFolder calendar;
        calendar.name = QStringLiteral("Calendar");
        calendar.container_class = QStringLiteral("IPF.Appointment");
        calendar.messages.append(event);
        PstParser parser;
        parser.open(writeStore(PstStoreBuilder{}.build({calendar})));
        const auto detail = onlyMessage(parser, QStringLiteral("Calendar"));
        QVERIFY(detail.has_value());
        QCOMPARE(detail->start_time, start);
        QCOMPARE(detail->end_time, end);
        QCOMPARE(detail->location, QStringLiteral("Front kitchen"));
        QCOMPARE(detail->busy_status, 2);
        QVERIFY(detail->is_all_day);
    }

    void fallsBackToTheStandardDatePropertiesForACalendarItem() {
        BuilderMessage event;
        event.message_class = QStringLiteral("IPM.Appointment");
        const QDateTime start(QDate(2026, 5, 1), QTime(8, 0), QTimeZone::UTC);
        event.extra_props = {{0x0060, sak::email::kPropTypeSysTime, builderFiletime(start)}};
        BuilderFolder calendar;
        calendar.name = QStringLiteral("Calendar");
        calendar.container_class = QStringLiteral("IPF.Appointment");
        calendar.messages.append(event);
        PstParser parser;
        parser.open(writeStore(PstStoreBuilder{}.build({calendar})));
        const auto detail = onlyMessage(parser, QStringLiteral("Calendar"));
        QVERIFY(detail.has_value());
        QCOMPARE(detail->start_time, start);
    }

    void readsTaskFieldsFromNamedProperties() {
        BuilderMessage task;
        task.message_class = QStringLiteral("IPM.Task");
        task.subject = QStringLiteral("Order flour");
        const QDateTime due(QDate(2026, 4, 20), QTime(0, 0), QTimeZone::UTC);
        task.named_props = {
            {kSetTask, sak::pst::kLidTaskStatus, sak::email::kPropTypeInt32, builderInt32(1)},
            {kSetTask,
             sak::pst::kLidTaskPercentComplete,
             sak::email::kPropTypeFloat64,
             builderDouble(0.5)},
            {kSetTask,
             sak::pst::kLidTaskDueDate,
             sak::email::kPropTypeSysTime,
             builderFiletime(due)},
        };
        BuilderFolder tasks;
        tasks.name = QStringLiteral("Tasks");
        tasks.container_class = QStringLiteral("IPF.Task");
        tasks.messages.append(task);
        PstParser parser;
        parser.open(writeStore(PstStoreBuilder{}.build({tasks})));
        const auto detail = onlyMessage(parser, QStringLiteral("Tasks"));
        QVERIFY(detail.has_value());
        QCOMPARE(detail->task_status, 1);
        QCOMPARE(detail->task_percent_complete, 0.5);
        QCOMPARE(detail->task_due_date, due);
    }

    void readsAContactsEmailAndANotesColour() {
        BuilderMessage contact;
        contact.message_class = QStringLiteral("IPM.Contact");
        contact.subject = QStringLiteral("Theo Brandt");
        contact.named_props = {{kSetAddress,
                                sak::pst::kLidEmail1EmailAddress,
                                sak::email::kPropTypeUnicode,
                                builderUnicode(QStringLiteral("theo@ovenworks.example"))}};
        BuilderFolder contacts;
        contacts.name = QStringLiteral("Contacts");
        contacts.container_class = QStringLiteral("IPF.Contact");
        contacts.messages.append(contact);

        BuilderMessage note;
        note.message_class = QStringLiteral("IPM.StickyNote");
        note.named_props = {
            {kSetNote, sak::pst::kLidNoteColor, sak::email::kPropTypeInt32, builderInt32(1)}};
        BuilderFolder notes;
        notes.name = QStringLiteral("Notes");
        notes.container_class = QStringLiteral("IPF.StickyNote");
        notes.messages.append(note);

        PstParser parser;
        parser.open(writeStore(PstStoreBuilder{}.build({contacts, notes})));
        const auto person = onlyMessage(parser, QStringLiteral("Contacts"));
        QVERIFY(person.has_value());
        QCOMPARE(person->email_address, QStringLiteral("theo@ovenworks.example"));
        const auto sticky = onlyMessage(parser, QStringLiteral("Notes"));
        QVERIFY(sticky.has_value());
        QCOMPARE(sticky->note_color, 1);
    }

    // Most calendar items, and some mail, have compressed RTF as their only body.
    void fillsTheBodyFromCompressedRtfWhenThatIsAllThereIs() {
        BuilderMessage msg = plainMessage(QStringLiteral("RTF only"));
        msg.body_plain.clear();
        msg.body_rtf_compressed =
            compressRtfLiterally("{\\rtf1\\ansi\\ansicpg1252\\pard Bring the invoice\\par}");
        BuilderFolder inbox;
        inbox.name = QStringLiteral("Inbox");
        inbox.messages.append(msg);
        PstParser parser;
        parser.open(writeStore(PstStoreBuilder{}.build({inbox})));
        const auto detail = onlyMessage(parser, QStringLiteral("Inbox"));
        QVERIFY(detail.has_value());
        QCOMPARE(detail->body_plain, QStringLiteral("Bring the invoice"));
    }

    void leavesADamagedRtfBodyUndecodedRatherThanGuessing() {
        BuilderMessage msg = plainMessage(QStringLiteral("Damaged RTF"));
        msg.body_plain.clear();
        msg.body_rtf_compressed = compressRtfLiterally("{\\rtf1 text}");
        msg.body_rtf_compressed[20] = 'Z';
        BuilderFolder inbox;
        inbox.name = QStringLiteral("Inbox");
        inbox.messages.append(msg);
        PstParser parser;
        parser.open(writeStore(PstStoreBuilder{}.build({inbox})));
        const auto detail = onlyMessage(parser, QStringLiteral("Inbox"));
        QVERIFY(detail.has_value());
        QVERIFY(detail->body_plain.isEmpty());
        QVERIFY(!detail->body_rtf_compressed.isEmpty());
    }

    // An attached email is a PtypObject. Its "data" was once the four bytes of the reference.
    void returnsAnAttachedEmailAsAnEml() {
        BuilderMessage inner = plainMessage(QStringLiteral("Original quote"));
        BuilderAttachment inner_file;
        inner_file.name = QStringLiteral("quote.txt");
        inner_file.data = "price list";
        inner.attachments.append(inner_file);
        BuilderMessage outer = plainMessage(QStringLiteral("Fwd: quote"));
        outer.attached_messages.append(inner);
        BuilderFolder inbox;
        inbox.name = QStringLiteral("Inbox");
        inbox.messages.append(outer);

        PstParser parser;
        parser.open(writeStore(PstStoreBuilder{}.build({inbox})));
        const auto detail = onlyMessage(parser, QStringLiteral("Inbox"));
        QVERIFY(detail.has_value());
        QCOMPARE(detail->attachments.size(), 1);
        const auto& att = detail->attachments.first();
        QVERIFY(att.is_embedded_message);
        QCOMPARE(att.mime_type, QStringLiteral("message/rfc822"));
        QCOMPARE(att.long_filename, QStringLiteral("Original quote.eml"));

        const auto data = parser.readAttachmentData(detail->node_id, att.index);
        QVERIFY(data.has_value());
        QVERIFY2(data->contains("Subject: Original quote"), "the EML carries the inner headers");
        QVERIFY2(data->contains("Body of Original quote"), "the EML carries the inner body");
        QVERIFY2(data->contains("quote.txt"), "the EML carries the inner attachment");
    }

    void returnsAnOleAttachmentsOwnStream() {
        BuilderMessage msg = plainMessage(QStringLiteral("With OLE"));
        BuilderAttachment ole;
        ole.name = QStringLiteral("chart.bin");
        ole.data = QByteArray("\xD0\xCF\x11\xE0 stream bytes", 17);
        ole.ole_object = true;
        msg.attachments.append(ole);
        BuilderFolder inbox;
        inbox.name = QStringLiteral("Inbox");
        inbox.messages.append(msg);
        PstParser parser;
        parser.open(writeStore(PstStoreBuilder{}.build({inbox})));
        const auto detail = onlyMessage(parser, QStringLiteral("Inbox"));
        QVERIFY(detail.has_value());
        const auto data = parser.readAttachmentData(detail->node_id, 0);
        QVERIFY(data.has_value());
        QCOMPARE(*data, ole.data);
    }

    // The list order once came from a hash seeded per run, so it changed each time the app started.
    void listsAttachmentsInTheOrderTheyWereAdded() {
        BuilderMessage msg = plainMessage(QStringLiteral("Many files"));
        constexpr int kFiles = 12;
        for (int i = 0; i < kFiles; ++i) {
            BuilderAttachment file;
            file.name = QStringLiteral("file%1.txt").arg(i);
            file.data = file.name.toLatin1();
            msg.attachments.append(file);
        }
        BuilderFolder inbox;
        inbox.name = QStringLiteral("Inbox");
        inbox.messages.append(msg);
        PstParser parser;
        parser.open(writeStore(PstStoreBuilder{}.build({inbox})));
        const auto detail = onlyMessage(parser, QStringLiteral("Inbox"));
        QVERIFY(detail.has_value());
        QCOMPARE(detail->attachments.size(), kFiles);
        for (int i = 0; i < kFiles; ++i) {
            const auto& att = detail->attachments.at(i);
            QCOMPARE(att.long_filename, QStringLiteral("file%1.txt").arg(i));
            QCOMPARE(att.index, i);
            const auto data = parser.readAttachmentData(detail->node_id, att.index);
            QVERIFY(data.has_value());
            QCOMPARE(*data, att.long_filename.toLatin1());
        }
    }

    void prefersTheSmtpAddressOverAnExchangePath() {
        BuilderMessage msg = plainMessage(QStringLiteral("From Exchange"));
        msg.sender_email = QStringLiteral("/O=ORG/OU=FIRST GROUP/CN=RECIPIENTS/CN=OWEN");
        msg.extra_props = {{0x5D01,
                            sak::email::kPropTypeUnicode,
                            builderUnicode(QStringLiteral("owen@harborlane.example"))}};
        BuilderFolder inbox;
        inbox.name = QStringLiteral("Inbox");
        inbox.messages.append(msg);
        PstParser parser;
        parser.open(writeStore(PstStoreBuilder{}.build({inbox})));
        const auto detail = onlyMessage(parser, QStringLiteral("Inbox"));
        QVERIFY(detail.has_value());
        QCOMPARE(detail->sender_email, QStringLiteral("owen@harborlane.example"));
    }

    void reassemblesABodyAndAttachmentLargerThanOneBlock() {
        BuilderMessage msg = plainMessage(QStringLiteral("Large"));
        msg.body_plain = QString(20'000, QLatin1Char('b'));
        BuilderAttachment file;
        file.name = QStringLiteral("big.bin");
        file.data = QByteArray(40'000, 'x');
        msg.attachments.append(file);
        BuilderFolder inbox;
        inbox.name = QStringLiteral("Inbox");
        inbox.messages.append(msg);
        PstParser parser;
        parser.open(writeStore(PstStoreBuilder{}.build({inbox})));
        const auto detail = onlyMessage(parser, QStringLiteral("Inbox"));
        QVERIFY(detail.has_value());
        QCOMPARE(detail->body_plain.size(), 20'000);
        const auto data = parser.readAttachmentData(detail->node_id, 0);
        QVERIFY(data.has_value());
        QCOMPARE(data->size(), 40'000);
    }
};

QTEST_MAIN(TestPstStoreStructure)
#include "test_pst_store_structure.moc"
