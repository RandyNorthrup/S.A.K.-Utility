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
#include "sak/pst_recurrence.h"

#include "../support/pst_store_builder.h"
#include "../support/recurrence_blob.h"

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
constexpr uint8_t kSetLog = 0x0A;
constexpr int kPageMetaOffset = kPageSize - kTrailerSize - kMetaPad - kMetaSize;
constexpr int kBtEntrySize = 24;
constexpr int kBtEntryBidOffset = 8;
constexpr int kBtEntryIbOffset = 16;
constexpr int kPageBidOffset = kPageSize - kTrailerSize + kPageTrailerBidOffset;

uint64_t le64At(const QByteArray& data, int offset) {
    uint64_t value = 0;
    std::memcpy(&value, data.constData() + offset, sizeof(value));
    return value;
}

/// A branch names the lowest key under it, so the last one not past the key is the way down.
int branchFor(const QByteArray& page, int entries, uint64_t key) {
    int chosen = 0;
    for (int i = 1; i < entries; ++i) {
        if (le64At(page, i * kBtEntrySize) <= key) {
            chosen = i;
        }
    }
    return chosen * kBtEntrySize;
}

/// Find a key the way a reader with no cache does: from the header's root, down the index pages by
/// btkey. The app's own parser walks every page and so never looks at a key; that is how a store
/// with blank keys passed every test here while libpff could find almost nothing in it.
bool foundByKey(const QByteArray& file, int root_field, int root_bid_field, uint64_t key) {
    uint64_t page_offset = le64At(file, kRootOffset + root_field);
    uint64_t expected_bid = le64At(file, kRootOffset + root_bid_field);
    for (int depth = 0; depth < 4; ++depth) {
        const QByteArray page = file.mid(static_cast<qsizetype>(page_offset), kPageSize);
        if (page.size() != kPageSize || le64At(page, kPageBidOffset) != expected_bid) {
            return false;  // The parent's BREF does not name this page
        }
        const int entries = static_cast<uint8_t>(page[kPageMetaOffset]);
        const int entry_size = static_cast<uint8_t>(page[kPageMetaOffset + 2]);
        const int level = static_cast<uint8_t>(page[kPageMetaOffset + 3]);
        if (level == 0) {
            for (int i = 0; i < entries; ++i) {
                if (le64At(page, i * entry_size) == key) {
                    return true;
                }
            }
            return false;
        }
        const int entry = branchFor(page, entries, key);
        page_offset = le64At(page, entry + kBtEntryIbOffset);
        expected_bid = le64At(page, entry + kBtEntryBidOffset);
    }
    return false;
}

/// Every BBT key, read off the leaf pages in order. @p ascending says whether they were in order;
/// @p internal_blocks, when given, receives where each internal block is in the file.
QVector<uint64_t> blockKeys(const QByteArray& file,
                            bool& ascending,
                            QVector<uint64_t>* internal_blocks = nullptr) {
    QVector<uint64_t> keys;
    ascending = true;
    const std::function<void(uint64_t)> walk = [&](uint64_t offset) {
        const QByteArray page = file.mid(static_cast<qsizetype>(offset), kPageSize);
        const int entries = static_cast<uint8_t>(page[kPageMetaOffset]);
        const int entry_size = static_cast<uint8_t>(page[kPageMetaOffset + 2]);
        if (static_cast<uint8_t>(page[kPageMetaOffset + 3]) != 0) {
            for (int i = 0; i < entries; ++i) {
                walk(le64At(page, (i * kBtEntrySize) + kBtEntryIbOffset));
            }
            return;
        }
        for (int i = 0; i < entries; ++i) {
            const uint64_t key = le64At(page, i * entry_size);
            ascending = ascending && (keys.isEmpty() || keys.last() < key);
            keys.append(key);
            if (internal_blocks != nullptr && (key & 0x02U) != 0) {
                internal_blocks->append(le64At(page, (i * entry_size) + kBbtEntryIbOffset));
            }
        }
    };
    walk(le64At(file, kRootOffset + kRootBbtField));
    return keys;
}

BuilderNamedProp appointmentProp(uint32_t long_id, uint16_t type, const QByteArray& value) {
    return BuilderNamedProp{
        .set_byte = kSetAppointment, .long_id = long_id, .type = type, .value = value};
}

/// The named properties every Outlook appointment has for its start and end.
QVector<BuilderNamedProp> appointmentTimes(const QDateTime& start, const QDateTime& end) {
    return {appointmentProp(sak::pst::kLidAppointmentStartWhole,
                            sak::email::kPropTypeSysTime,
                            builderFiletime(start)),
            appointmentProp(sak::pst::kLidAppointmentEndWhole,
                            sak::email::kPropTypeSysTime,
                            builderFiletime(end))};
}

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

    // libpff listed only the root of a store built here: the builder left the index pages' keys
    // blank and its B-trees out of order, and its own parser, which walks every page, never
    // noticed.
    void everyNodeAndBlockIsFoundByItsKey() {
        QVector<BuilderMessage> messages;
        for (int i = 0; i < 30; ++i) {
            BuilderMessage msg = plainMessage(QStringLiteral("Message %1").arg(i));
            if (i % 5 == 0) {
                msg.attachments = {{QStringLiteral("f.bin"), {}, QByteArray("data"), {}}};
            }
            messages.append(msg);
        }
        BuilderFolder inbox;
        inbox.name = QStringLiteral("Inbox");
        inbox.messages = messages;
        const QByteArray store = PstStoreBuilder{}.build({inbox});
        // The point is an index page, so the store has to outgrow one leaf.
        QVERIFY(static_cast<uint8_t>(
                    store[static_cast<qsizetype>(le64At(store, kRootOffset + kRootNbtField)) +
                          kPageMetaOffset + 3]) == 1);

        PstParser parser;
        parser.open(writeStore(store));
        QVERIFY(parser.isOpen());
        const QVector<uint64_t> nids = parser.allNodeIds();
        QVERIFY(nids.size() > kBuilderNbtPerPage);
        for (const uint64_t nid : nids) {
            QVERIFY2(foundByKey(store, kRootNbtField, kRootNbtBidField, nid),
                     qPrintable(QStringLiteral("NID 0x%1").arg(nid, 0, 16)));
        }

        bool ascending = false;
        const QVector<uint64_t> bids = blockKeys(store, ascending);
        QVERIFY(ascending);
        QVERIFY(bids.size() > kBuilderBbtPerPage);
        for (const uint64_t bid : bids) {
            QVERIFY2(foundByKey(store, kRootBbtField, kRootBbtBidField, bid),
                     qPrintable(QStringLiteral("BID 0x%1").arg(bid, 0, 16)));
        }
    }

    // The tables libpff and Outlook reach a message's parts through, and the map's other two
    // properties. The parser here finds attachments by walking sub-nodes, so it never needed them.
    void aStoreHasTheTablesItsSpecificationNames() {
        BuilderMessage msg = plainMessage(QStringLiteral("With parts"));
        msg.attachments = {{QStringLiteral("f.bin"), {}, QByteArray("data"), {}}};
        msg.recipients = {{QStringLiteral("Dana"), QStringLiteral("dana@harborlane.example")}};
        BuilderFolder inbox;
        inbox.name = QStringLiteral("Inbox");
        inbox.messages = {msg};
        const QByteArray store = PstStoreBuilder{}.build({inbox});

        // Sub-node blocks are B-tree leaves: both tables there, in NID order.
        bool ascending = false;
        QVector<uint64_t> internal_blocks;
        (void)blockKeys(store, ascending, &internal_blocks);
        QVector<uint64_t> table_nids;
        bool nids_ascending = true;
        for (const uint64_t at : internal_blocks) {
            const auto start = static_cast<int>(at);
            if (static_cast<uint8_t>(store[start]) != kSubnodeBlockBType || store[start + 1] != 0) {
                continue;
            }
            const int entries = static_cast<uint8_t>(store[start + kSubnodeCEntOffset]);
            uint64_t previous = 0;
            for (int i = 0; i < entries; ++i) {
                const uint64_t nid = le64At(store,
                                            start + kSubnodeBlockHeaderLen + (i * kSlEntryLen));
                nids_ascending = nids_ascending && nid > previous;
                previous = nid;
                table_nids.append(nid);
            }
        }
        QVERIFY(nids_ascending);
        QVERIFY(table_nids.contains(kBuilderNidAttachmentTable));
        QVERIFY(table_nids.contains(kBuilderNidRecipientTable));

        PstParser parser;
        parser.open(writeStore(store));
        QVERIFY(parser.isOpen());
        const auto map = parser.readItemProperties(sak::email::kNidNameToIdMap);
        QVERIFY(map.has_value());
        QStringList present;
        for (const auto& prop : *map) {
            present.append(QString::number(prop.tag_id));
        }
        for (const uint16_t id : {kBuilderPropNameidBucketCount,
                                  sak::pst::kPropIdNameidStreamGuid,
                                  sak::pst::kPropIdNameidStreamEntry,
                                  kBuilderPropNameidStreamString}) {
            QVERIFY2(present.contains(QString::number(id)), qPrintable(QString::number(id)));
        }
        const auto parts = onlyMessage(parser, QStringLiteral("Inbox"));
        QVERIFY(parts.has_value());
        QCOMPARE(parts->attachments.size(), 1);
    }

    void anEventsAttendeesComeFromItsRecipientTable() {
        BuilderRecipient owner{QStringLiteral("Dana Whitlock"),
                               QStringLiteral("dana@harborlane.example")};
        owner.organizer = true;
        BuilderRecipient required{QStringLiteral("Marisol Quint"),
                                  QStringLiteral("marisol@harborlane.example")};
        required.track_status = 3;
        BuilderRecipient optional{QStringLiteral("Owen Lark")};
        optional.email_address = QStringLiteral("owen@harborlane.example");
        optional.recipient_type = 2;
        optional.track_status = 4;
        BuilderRecipient exchange{QStringLiteral("Theo Brandt")};
        exchange.email_address = QStringLiteral("/o=Harbor/ou=First/cn=Recipients/cn=theo");
        exchange.address_type = QStringLiteral("EX");
        BuilderRecipient resource{QStringLiteral("Back room")};
        resource.recipient_type = 3;
        resource.address_type.clear();

        BuilderMessage event;
        event.message_class = QStringLiteral("IPM.Appointment");
        event.subject = QStringLiteral("Staff meeting");
        event.recipients = {owner, required, optional, exchange, resource};
        BuilderFolder calendar;
        calendar.name = QStringLiteral("Calendar");
        calendar.container_class = QStringLiteral("IPF.Appointment");
        calendar.messages = {event};

        PstParser parser;
        parser.open(writeStore(PstStoreBuilder{}.build({calendar})));
        const auto detail = onlyMessage(parser, QStringLiteral("Calendar"));
        QVERIFY(detail.has_value());
        QVERIFY(!detail->attendees_unreadable);
        QCOMPARE(detail->attendees.size(), 5);
        QCOMPARE(detail->attendees[0].name, QStringLiteral("Dana Whitlock"));
        QCOMPARE(detail->attendees[0].email, QStringLiteral("dana@harborlane.example"));
        QVERIFY(detail->attendees[0].organizer);
        QCOMPARE(detail->attendees[1].response, 3);
        QVERIFY(!detail->attendees[1].organizer);
        QCOMPARE(detail->attendees[2].role, 2);
        QCOMPARE(detail->attendees[2].email, QStringLiteral("owen@harborlane.example"));
        QCOMPARE(detail->attendees[2].response, 4);
        // An Exchange path is no address any other program can use.
        QCOMPARE(detail->attendees[3].name, QStringLiteral("Theo Brandt"));
        QVERIFY(detail->attendees[3].email.isEmpty());
        QCOMPARE(detail->attendees[4].role, 3);
    }

    // An email's recipients are in its To and Cc lines. Reading the table for 16,000 of them buys
    // nothing, and a damaged one must not cost the message.
    void anEmailsRecipientTableIsLeftAlone() {
        BuilderMessage msg = plainMessage(QStringLiteral("Note"));
        msg.damaged_recipient_table = true;
        BuilderFolder inbox;
        inbox.name = QStringLiteral("Inbox");
        inbox.messages = {msg};
        PstParser parser;
        parser.open(writeStore(PstStoreBuilder{}.build({inbox})));
        const auto detail = onlyMessage(parser, QStringLiteral("Inbox"));
        QVERIFY(detail.has_value());
        QVERIFY(detail->attendees.isEmpty());
        QVERIFY(!detail->attendees_unreadable);
    }

    void aDamagedRecipientTableIsNotedAndTheEventStillOpens() {
        BuilderMessage event;
        event.message_class = QStringLiteral("IPM.Appointment");
        event.subject = QStringLiteral("Staff meeting");
        event.damaged_recipient_table = true;
        BuilderFolder calendar;
        calendar.name = QStringLiteral("Calendar");
        calendar.container_class = QStringLiteral("IPF.Appointment");
        calendar.messages = {event};
        PstParser parser;
        parser.open(writeStore(PstStoreBuilder{}.build({calendar})));
        const auto detail = onlyMessage(parser, QStringLiteral("Calendar"));
        QVERIFY(detail.has_value());
        QCOMPARE(detail->subject, QStringLiteral("Staff meeting"));
        QVERIFY(detail->attendees.isEmpty());
        QVERIFY(detail->attendees_unreadable);
    }

    void aRepeatingEventReadsItsPatternAndZone() {
        RecurrenceSpec spec;
        spec.start_date = QDate(2026, 11, 2);
        spec.deleted = {QDate(2026, 11, 9)};

        BuilderMessage event;
        event.message_class = QStringLiteral("IPM.Appointment");
        event.subject = QStringLiteral("Staff meeting");
        const QDateTime start(QDate(2026, 11, 2), QTime(17, 0), QTimeZone::UTC);
        event.named_props = appointmentTimes(start, start.addSecs(3600));
        event.named_props.append(appointmentProp(sak::pst::kLidAppointmentRecur,
                                                 sak::email::kPropTypeBinary,
                                                 buildRecurrenceBlob(spec)));
        event.named_props.append(appointmentProp(
            sak::pst::kLidTimeZoneStruct, sak::email::kPropTypeBinary, pacificZoneStruct()));
        event.named_props.append(appointmentProp(sak::pst::kLidTimeZoneDescription,
                                                 sak::email::kPropTypeUnicode,
                                                 builderUnicode(QStringLiteral("Pacific"))));
        BuilderFolder calendar;
        calendar.name = QStringLiteral("Calendar");
        calendar.container_class = QStringLiteral("IPF.Appointment");
        calendar.messages = {event};

        PstParser parser;
        parser.open(writeStore(PstStoreBuilder{}.build({calendar})));
        const auto detail = onlyMessage(parser, QStringLiteral("Calendar"));
        QVERIFY(detail.has_value());
        QCOMPARE(detail->recurrence.state, sak::RecurrenceState::Readable);
        QCOMPARE(detail->recurrence.rrule,
                 QStringLiteral("FREQ=WEEKLY;INTERVAL=1;BYDAY=MO;WKST=SU"));
        QCOMPARE(detail->recurrence.deleted.size(), 1);
        QVERIFY(detail->time_zone.known);
        QCOMPARE(detail->time_zone.name, QStringLiteral("Pacific"));
        // The pattern's wall clock and the event's start name the same moment.
        QCOMPARE(sak::pst::zoneWallClockToUtc(detail->time_zone, detail->recurrence.wall_start),
                 start);
    }

    void theSeriesZoneWinsOverTheOneItsStartWasShownIn() {
        BuilderMessage event;
        event.message_class = QStringLiteral("IPM.Appointment");
        event.subject = QStringLiteral("Zones");
        event.named_props = {
            appointmentProp(sak::pst::kLidTimeZoneDefinitionStart,
                            sak::email::kPropTypeBinary,
                            buildTimeZoneDefinition(QStringLiteral("Start Zone"), 0, 0, {}, {})),
            appointmentProp(
                sak::pst::kLidTimeZoneStruct, sak::email::kPropTypeBinary, pacificZoneStruct()),
            appointmentProp(sak::pst::kLidTimeZoneDescription,
                            sak::email::kPropTypeUnicode,
                            builderUnicode(QStringLiteral("Struct Zone"))),
            appointmentProp(sak::pst::kLidTimeZoneDefinitionRecur,
                            sak::email::kPropTypeBinary,
                            buildTimeZoneDefinition(QStringLiteral("Series Zone"), 0, 0, {}, {})),
        };
        BuilderMessage struct_only = event;
        struct_only.named_props.removeLast();
        BuilderMessage start_only = event;
        start_only.named_props = {event.named_props.first()};
        BuilderFolder calendar;
        calendar.name = QStringLiteral("Calendar");
        calendar.container_class = QStringLiteral("IPF.Appointment");
        calendar.messages = {event, struct_only, start_only};

        PstParser parser;
        parser.open(writeStore(PstStoreBuilder{}.build({calendar})));
        const auto* folder = folderNamed(parser.folderTree(), QStringLiteral("Calendar"));
        QVERIFY(folder != nullptr);
        const auto items = parser.readFolderItems(folder->node_id, 0, 0);
        QVERIFY(items.has_value());
        QCOMPARE(items->size(), 3);
        QCOMPARE(parser.readItemDetail(items->at(0).node_id)->time_zone.name,
                 QStringLiteral("Series Zone"));
        QCOMPARE(parser.readItemDetail(items->at(1).node_id)->time_zone.name,
                 QStringLiteral("Struct Zone"));
        QCOMPARE(parser.readItemDetail(items->at(2).node_id)->time_zone.name,
                 QStringLiteral("Start Zone"));
    }

    void anEventMarkedRepeatingWithNoPatternSaysSo() {
        BuilderMessage event;
        event.message_class = QStringLiteral("IPM.Appointment");
        event.subject = QStringLiteral("No pattern");
        event.named_props = {appointmentProp(
            sak::pst::kLidRecurring, sak::email::kPropTypeBoolean, QByteArray(1, 1))};
        BuilderMessage single;
        single.message_class = QStringLiteral("IPM.Appointment");
        single.subject = QStringLiteral("Single");
        single.named_props = {appointmentProp(
            sak::pst::kLidRecurring, sak::email::kPropTypeBoolean, QByteArray(1, 0))};
        BuilderFolder calendar;
        calendar.name = QStringLiteral("Calendar");
        calendar.container_class = QStringLiteral("IPF.Appointment");
        calendar.messages = {event, single};
        PstParser parser;
        parser.open(writeStore(PstStoreBuilder{}.build({calendar})));
        const auto* folder = folderNamed(parser.folderTree(), QStringLiteral("Calendar"));
        const auto items = parser.readFolderItems(folder->node_id, 0, 0);
        QVERIFY(items.has_value());
        QCOMPARE(parser.readItemDetail(items->at(0).node_id)->recurrence.state,
                 sak::RecurrenceState::Unreadable);
        QCOMPARE(parser.readItemDetail(items->at(1).node_id)->recurrence.state,
                 sak::RecurrenceState::None);
    }

    void aJournalEntryReadsItsFields() {
        const auto logged = [](uint32_t long_id, uint16_t type, const QByteArray& value) {
            return BuilderNamedProp{
                .set_byte = kSetLog, .long_id = long_id, .type = type, .value = value};
        };
        const QDateTime start(QDate(2026, 3, 9), QTime(14, 0), QTimeZone::UTC);
        BuilderMessage entry;
        entry.message_class = QStringLiteral("IPM.Activity");
        entry.subject = QStringLiteral("Call with the oven company");
        entry.body_plain = QStringLiteral("Thermostat part on order.");
        entry.named_props = {
            logged(sak::pst::kLidLogType,
                   sak::email::kPropTypeUnicode,
                   builderUnicode(QStringLiteral("Phone call"))),
            logged(sak::pst::kLidLogStart, sak::email::kPropTypeSysTime, builderFiletime(start)),
            logged(sak::pst::kLidLogEnd,
                   sak::email::kPropTypeSysTime,
                   builderFiletime(start.addSecs(900))),
            logged(sak::pst::kLidLogDuration, sak::email::kPropTypeInt32, builderInt32(15)),
        };
        BuilderFolder journal;
        journal.name = QStringLiteral("Journal");
        journal.container_class = QStringLiteral("IPF.Journal");
        journal.messages = {entry};
        PstParser parser;
        parser.open(writeStore(PstStoreBuilder{}.build({journal})));
        const auto detail = onlyMessage(parser, QStringLiteral("Journal"));
        QVERIFY(detail.has_value());
        QCOMPARE(detail->item_type, sak::EmailItemType::JournalEntry);
        QCOMPARE(detail->journal_type, QStringLiteral("Phone call"));
        QCOMPARE(detail->start_time, start);
        QCOMPARE(detail->end_time, start.addSecs(900));
        QCOMPARE(detail->journal_duration_minutes, 15);
    }

    void aClassOnlyExtendsAtADot_data() {
        QTest::addColumn<QString>("message_class");
        QTest::addColumn<int>("type");
        const auto row = [](const char* name, sak::EmailItemType type) {
            QTest::newRow(name) << QString::fromLatin1(name) << static_cast<int>(type);
        };
        row("IPM.Note", sak::EmailItemType::Email);
        row("IPM.Note.SMIME.MultipartSigned", sak::EmailItemType::Email);
        row("IPM.Note.Microsoft.Voicemail.UM", sak::EmailItemType::Email);
        row("IPM.Post", sak::EmailItemType::Email);
        row("REPORT.IPM.Note.NDR", sak::EmailItemType::Email);
        row("IPM.Sharing", sak::EmailItemType::Email);
        row("IPM.TaskRequest", sak::EmailItemType::Email);
        row("IPM.Task", sak::EmailItemType::Task);
        row("IPM.Schedule.Meeting.Request", sak::EmailItemType::MeetingRequest);
        row("IPM.Activity", sak::EmailItemType::JournalEntry);
        row("IPM.Note.Rules.OofTemplate.Microsoft", sak::EmailItemType::Unknown);
        row("IPM.Configuration.Calendar", sak::EmailItemType::Unknown);
        row("IPM.Microsoft.FolderDesign.NamedView", sak::EmailItemType::Unknown);
        row("IPM.ConversationAction", sak::EmailItemType::Unknown);
        row("IPM.AbchPerson", sak::EmailItemType::Unknown);
        row("IPM", sak::EmailItemType::Unknown);
        // Not on the list, so not called mail: it is counted as left out and opens in its folder.
        row("IPM.SomethingNew.Entirely", sak::EmailItemType::Unknown);
    }

    void aClassOnlyExtendsAtADot() {
        QFETCH(QString, message_class);
        QFETCH(int, type);
        QCOMPARE(static_cast<int>(PstParser::classifyMessageClass(message_class)), type);
    }

    void systemFoldersAreCountedByTheRealEmailsInThem() {
        BuilderFolder history;
        history.name = QStringLiteral("Conversation History");
        history.messages = {plainMessage(QStringLiteral("Chat")),
                            plainMessage(QStringLiteral("Chat 2"))};
        BuilderMessage setting;
        setting.message_class = QStringLiteral("IPM.Configuration.Mine");
        setting.subject = QStringLiteral("Settings");
        BuilderFolder files;
        files.name = QStringLiteral("Files");
        files.messages = {setting};
        BuilderFolder sync;
        sync.name = QStringLiteral("Sync Issues");
        BuilderFolder conflicts;
        conflicts.name = QStringLiteral("Conflicts");
        conflicts.messages = {plainMessage(QStringLiteral("Conflict"))};
        sync.children = {conflicts};
        BuilderFolder inbox;
        inbox.name = QStringLiteral("Inbox");
        inbox.messages = {plainMessage(QStringLiteral("Ordinary"))};
        BuilderFolder outside;
        outside.name = QStringLiteral("Outside");
        outside.messages = {plainMessage(QStringLiteral("Stray"))};

        PstStoreBuilder builder;
        builder.outside_folders = {outside};
        PstParser parser;
        parser.open(writeStore(builder.build({inbox, history, files, sync})));
        QVERIFY(parser.isOpen());
        const auto tree = parser.folderTree();
        QCOMPARE(folderNamed(tree, QStringLiteral("Conversation History"))->system_folder_emails,
                 2);
        QCOMPARE(folderNamed(tree, QStringLiteral("Files"))->system_folder_emails, 0);
        QCOMPARE(folderNamed(tree, QStringLiteral("Sync Issues"))->system_folder_emails, 0);
        QCOMPARE(folderNamed(tree, QStringLiteral("Conflicts"))->system_folder_emails, 1);
        QCOMPARE(folderNamed(tree, QStringLiteral("Outside"))->system_folder_emails, 1);
        // An ordinary mail folder is on show anyway, so nothing is counted for it.
        QCOMPARE(folderNamed(tree, QStringLiteral("Inbox"))->system_folder_emails, 0);
    }

    // Past a few hundred messages the rows no longer fit in the table's heap. Outlook keeps them in
    // a sub-node over several blocks, each padded to its end, so reading the bytes straight through
    // would land inside the padding and call the rest of the folder garbage.
    void aFolderOfHundredsOfMessagesListsEveryOne() {
        constexpr int kMessages = 320;
        BuilderFolder inbox;
        inbox.name = QStringLiteral("Inbox");
        inbox.spanning_table = true;
        QStringList expected;
        for (int i = 0; i < kMessages; ++i) {
            expected.append(QStringLiteral("Message %1").arg(i, 3, 10, QLatin1Char('0')));
            inbox.messages.append(plainMessage(expected.last()));
        }
        PstParser parser;
        parser.open(writeStore(PstStoreBuilder{}.build({inbox})));
        QVERIFY(parser.isOpen());
        const auto* folder = folderNamed(parser.folderTree(), QStringLiteral("Inbox"));
        QVERIFY(folder != nullptr);
        QCOMPARE(folder->content_count, kMessages);

        const auto items = parser.readFolderItems(folder->node_id, 0, 0);
        QVERIFY(items.has_value());
        QCOMPARE(static_cast<int>(items->size()), kMessages);
        QStringList seen;
        for (const auto& item : *items) {
            const auto detail = parser.readItemDetail(item.node_id);
            QVERIFY(detail.has_value());
            seen.append(detail->subject);
        }
        seen.sort();
        QCOMPARE(seen, expected);

        // A page that starts in the last block, past two blocks' padding.
        const auto last_page = parser.readFolderItems(folder->node_id, kMessages - 10, 50);
        QVERIFY(last_page.has_value());
        QCOMPARE(static_cast<int>(last_page->size()), 10);
    }

    // A table that leaves out the class column says nothing about what the row is.
    void aTableWithNoClassColumnIsNotTakenForBookkeeping() {
        BuilderFolder inbox;
        inbox.name = QStringLiteral("Inbox");
        inbox.messages = {plainMessage(QStringLiteral("One"))};
        inbox.minimal_table = true;
        PstParser parser;
        parser.open(writeStore(PstStoreBuilder{}.build({inbox})));
        const auto* folder = folderNamed(parser.folderTree(), QStringLiteral("Inbox"));
        const auto items = parser.readFolderItems(folder->node_id, 0, 0);
        QVERIFY(items.has_value());
        QCOMPARE(items->first().item_type, sak::EmailItemType::Email);
    }

    void aStoreIsOnlyCalledEmptyAfterLookingInIt() {
        BuilderFolder empty;
        empty.name = QStringLiteral("Inbox");
        PstParser none;
        none.open(writeStore(PstStoreBuilder{}.build({empty})));
        QVERIFY(none.isOpen());
        QCOMPARE(none.fileInfo().total_items, 0);

        BuilderFolder undeclared;
        undeclared.name = QStringLiteral("Inbox");
        undeclared.messages = {plainMessage(QStringLiteral("Hidden"))};
        undeclared.declared_count = 0;
        PstParser some;
        some.open(writeStore(PstStoreBuilder{}.build({undeclared})));
        QVERIFY(some.isOpen());
        QCOMPARE(some.fileInfo().total_items, 1);
    }
};

QTEST_MAIN(TestPstStoreStructure)
#include "test_pst_store_structure.moc"
