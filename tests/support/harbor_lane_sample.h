// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file harbor_lane_sample.h
/// @brief The fictional mailbox behind the wiki's Email Tools screenshots.
///
/// Everything here is invented: Harbor Lane Bakery, its people, its mail. The addresses are on the
/// reserved .example domain and the phone numbers are in the 555-01xx range kept for fiction. No
/// byte of it comes from a real mailbox, so it can be shown, shared and published freely. The spec
/// is the "needs" text of email_inspector_message in the site's wiki/shots/email-tools.json.

#pragma once

#include "sak/pst_named_properties.h"

#include "pst_store_builder.h"
#include "recurrence_blob.h"

#include <QBuffer>
#include <QColor>
#include <QImage>
#include <QPainter>

namespace sak::pst_fixture::harbor_lane {

inline constexpr uint8_t kSetAppointment = 0x02;
inline constexpr uint8_t kSetTask = 0x03;
inline constexpr uint8_t kSetAddress = 0x04;
inline constexpr uint8_t kSetNote = 0x0E;
inline constexpr uint16_t kPropTransportHeaders = sak::email::kPropIdTransportHeaders;

inline const QString kOwnerName = QStringLiteral("Dana Whitlock");
inline const QString kOwnerAddress = QStringLiteral("dana@harborlane.example");

/// A small but genuine PDF: one page carrying @p text.
inline QByteArray minimalPdf(const QString& text) {
    const QByteArray stream = "BT /F1 18 Tf 72 720 Td (" + text.toLatin1() + ") Tj ET";
    QVector<QByteArray> objects{
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R "
        "/Resources << /Font << /F1 5 0 R >> >> >>",
        "<< /Length " + QByteArray::number(stream.size()) + " >>\nstream\n" + stream +
            "\nendstream",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    };
    QByteArray pdf = "%PDF-1.4\n";
    QVector<qsizetype> offsets;
    for (int i = 0; i < objects.size(); ++i) {
        offsets.append(pdf.size());
        pdf += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const qsizetype xref = pdf.size();
    pdf += "xref\n0 " + QByteArray::number(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (const qsizetype offset : offsets) {
        pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size " + QByteArray::number(objects.size() + 1) +
           " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    return pdf;
}

/// A picture of a shop front, drawn here, so no photograph of anywhere real is involved.
inline QByteArray drawnJpeg(const QString& caption, const QColor& wall, const QColor& awning) {
    constexpr int kWidth = 640;
    constexpr int kHeight = 420;
    QImage image(kWidth, kHeight, QImage::Format_RGB32);
    image.fill(wall);
    QPainter painter(&image);
    painter.fillRect(60, 40, 520, 60, awning);
    painter.fillRect(90, 140, 180, 240, QColor(70, 90, 110));
    painter.fillRect(330, 140, 220, 160, QColor(190, 215, 230));
    painter.setPen(Qt::white);
    painter.drawText(QRect(60, 40, 520, 60), Qt::AlignCenter, caption);
    painter.end();
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG");
    return bytes;
}

/// A .docx is a ZIP. These are the stored entries a word processor needs to open it.
inline QByteArray minimalDocx(const QString& text) {
    const auto crc32 = [](const QByteArray& data) {
        uint32_t crc = 0xFF'FF'FF'FFU;
        for (const char byte : data) {
            crc ^= static_cast<uint8_t>(byte);
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc & 1U) != 0 ? (0xED'B8'83'20U ^ (crc >> 1)) : (crc >> 1);
            }
        }
        return ~crc;
    };
    const QVector<QPair<QByteArray, QByteArray>> entries{
        {"[Content_Types].xml",
         "<?xml version=\"1.0\"?><Types xmlns=\"http://schemas.openxmlformats.org/package/2006/"
         "content-types\"><Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-"
         "package.relationships+xml\"/><Default Extension=\"xml\" ContentType=\"application/vnd."
         "openxmlformats-officedocument.wordprocessingml.document.main+xml\"/></Types>"},
        {"_rels/.rels",
         "<?xml version=\"1.0\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/"
         "2006/relationships\"><Relationship Id=\"r1\" Type=\"http://schemas.openxmlformats.org/"
         "officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
         "</Relationships>"},
        {"word/document.xml",
         "<?xml version=\"1.0\"?><w:document xmlns:w=\"http://schemas.openxmlformats.org/"
         "wordprocessingml/2006/main\"><w:body><w:p><w:r><w:t>" +
             text.toUtf8() + "</w:t></w:r></w:p></w:body></w:document>"},
    };
    QByteArray body;
    QByteArray central;
    const auto le16 = [](uint32_t value) {
        QByteArray out(2, '\0');
        writeLe16(out, 0, static_cast<uint16_t>(value));
        return out;
    };
    const auto le32 = [](uint32_t value) {
        QByteArray out(4, '\0');
        writeLe32(out, 0, value);
        return out;
    };
    for (const auto& [name, data] : entries) {
        const auto offset = static_cast<uint32_t>(body.size());
        const auto size = static_cast<uint32_t>(data.size());
        const QByteArray shared = le16(20) + le16(0) + le16(0) + le16(0) + le16(0) +
                                  le32(crc32(data)) + le32(size) + le32(size) +
                                  le16(static_cast<uint32_t>(name.size())) + le16(0);
        body += QByteArray("PK\x03\x04", 4) + shared + name + data;
        central += QByteArray("PK\x01\x02", 4) + le16(20) + shared + le16(0) + le16(0) + le16(0) +
                   le32(0) + le32(offset) + name;
    }
    const auto central_offset = static_cast<uint32_t>(body.size());
    const auto count = static_cast<uint32_t>(entries.size());
    return body + central + QByteArray("PK\x05\x06", 4) + le16(0) + le16(0) + le16(count) +
           le16(count) + le32(static_cast<uint32_t>(central.size())) + le32(central_offset) +
           le16(0);
}

inline QString transportHeaders(const BuilderMessage& msg, const QString& to_address) {
    return QStringLiteral(
               "Received: from mail.harborlane.example by mx.harborlane.example; %1\r\n"
               "From: %2 <%3>\r\nTo: %4\r\nSubject: %5\r\nDate: %1\r\n"
               "Message-ID: <%6>\r\nMIME-Version: 1.0\r\nContent-Type: text/html; "
               "charset=\"utf-8\"\r\n")
        .arg(msg.date.toString(Qt::RFC2822Date),
             msg.sender_name,
             msg.sender_email,
             to_address,
             msg.subject,
             msg.internet_message_id);
}

inline BuilderMessage mail(const QString& subject,
                           const QString& from_name,
                           const QString& from_address,
                           const QDateTime& when,
                           const QString& text) {
    BuilderMessage msg;
    msg.subject = subject;
    msg.sender_name = from_name;
    msg.sender_email = from_address;
    msg.display_to = kOwnerName;
    msg.date = when;
    msg.body_plain = text;
    QString html = text.toHtmlEscaped();
    html.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    msg.body_html = QStringLiteral("<html><body style=\"font-family:Segoe UI,sans-serif\"><p>") +
                    html + QStringLiteral("</p></body></html>");
    msg.internet_message_id =
        QStringLiteral("%1.%2@harborlane.example").arg(when.toSecsSinceEpoch()).arg(qHash(subject));
    msg.extra_props = {{kPropTransportHeaders,
                        sak::email::kPropTypeUnicode,
                        builderUnicode(transportHeaders(msg, kOwnerAddress))}};
    return msg;
}

inline QDateTime at(int month, int day, int hour, int minute = 0) {
    return QDateTime(QDate(2026, month, day), QTime(hour, minute), QTimeZone::UTC);
}

inline BuilderMessage springMenuMail() {
    BuilderMessage menu =
        mail(QStringLiteral("Spring menu draft"),
             QStringLiteral("Marisol Quint"),
             QStringLiteral("marisol@harborlane.example"),
             at(3, 9, 14, 5),
             QStringLiteral("Hi Dana,\n\nHere is the spring menu draft. I moved the "
                            "lemon tart up and added the rye loaf. Can you check "
                            "the prices on page two before Friday?\n\nMarisol"));
    menu.read = false;
    menu.attachments = {{QStringLiteral("spring_menu.pdf"),
                         QStringLiteral("application/pdf"),
                         minimalPdf(QStringLiteral("Harbor Lane Bakery - Spring Menu Draft")),
                         {}}};
    return menu;
}

inline BuilderMessage ovenQuoteMail() {
    BuilderMessage quote = mail(QStringLiteral("Oven repair quote"),
                                QStringLiteral("Theo Brandt"),
                                QStringLiteral("theo@ovenworks.example"),
                                at(3, 11, 9, 40),
                                QStringLiteral("Dana,\n\nQuote attached for the deck oven. The "
                                               "thermostat and door seal both need replacing. We "
                                               "can come out next Tuesday if that suits.\n\nTheo "
                                               "Brandt, Oven Works"));
    quote.attachments = {{QStringLiteral("oven_quote.docx"),
                          QStringLiteral("application/vnd.openxmlformats-officedocument."
                                         "wordprocessingml.document"),
                          minimalDocx(QStringLiteral("Oven Works quote: deck oven thermostat and "
                                                     "door seal.")),
                          {}}};
    return quote;
}

inline BuilderMessage invoiceMail() {
    BuilderMessage invoice = mail(QStringLiteral("Invoice 1042 for flour delivery"),
                                  QStringLiteral("Priya Venn"),
                                  QStringLiteral("billing@millstonefoods.example"),
                                  at(3, 12, 8, 15),
                                  QStringLiteral("Hello,\n\nInvoice 1042 is attached for last "
                                                 "week's flour delivery. Payment is due in 30 "
                                                 "days.\n\nPriya Venn, Millstone Foods"));
    invoice.attachments = {{QStringLiteral("invoice_1042.pdf"),
                            QStringLiteral("application/pdf"),
                            minimalPdf(QStringLiteral("Millstone Foods - Invoice 1042")),
                            {}}};
    return invoice;
}

inline BuilderMessage storefrontMail() {
    BuilderMessage photos =
        mail(QStringLiteral("Storefront photos"),
             QStringLiteral("Marisol Quint"),
             QStringLiteral("marisol@harborlane.example"),
             at(3, 13, 16, 30),
             QStringLiteral("The sign painter finished this morning. Two photos "
                            "attached: the storefront and the new awning.\n\n"
                            "Marisol"));
    photos.attachments = {
        {QStringLiteral("storefront.jpg"),
         QStringLiteral("image/jpeg"),
         drawnJpeg(
             QStringLiteral("Harbor Lane Bakery"), QColor(214, 196, 170), QColor(160, 60, 50)),
         {}},
        {QStringLiteral("awning.jpg"),
         QStringLiteral("image/jpeg"),
         drawnJpeg(QStringLiteral("New awning"), QColor(190, 205, 190), QColor(40, 90, 120)),
         {}}};
    return photos;
}

inline BuilderFolder inboxFolder() {
    BuilderMessage schedule =
        mail(QStringLiteral("Saturday staff schedule"),
             QStringLiteral("Owen Lark"),
             QStringLiteral("owen@harborlane.example"),
             at(3, 16, 7, 50),
             QStringLiteral("Dana,\n\nI can open on Saturday if Marisol takes "
                            "the afternoon. Does that work?\n\nOwen"));
    BuilderMessage market =
        mail(QStringLiteral("Farmers market booth"),
             QStringLiteral("Ines Caldera"),
             QStringLiteral("ines@portmarket.example"),
             at(3, 18, 11, 20),
             QStringLiteral("Hi Dana,\n\nA corner booth opened up for the spring "
                            "market. Let me know by Friday if you would like "
                            "it.\n\nInes Caldera, Port Market"));
    BuilderFolder folder;
    folder.name = QStringLiteral("Inbox");
    folder.messages = {
        springMenuMail(), ovenQuoteMail(), invoiceMail(), storefrontMail(), schedule, market};
    return folder;
}

inline BuilderFolder draftsFolder() {
    BuilderMessage draft;
    draft.subject = QStringLiteral("Re: Oven repair quote");
    draft.display_to = QStringLiteral("Theo Brandt");
    draft.body_plain =
        QStringLiteral("Theo, Tuesday works. Please come before the morning bake if");
    draft.created = at(3, 11, 12, 5);
    BuilderFolder folder;
    folder.name = QStringLiteral("Drafts");
    folder.messages = {draft};
    return folder;
}

inline BuilderFolder sentFolder() {
    const auto sent =
        [](const QString& subject, const QString& to, const QDateTime& when, const QString& text) {
            BuilderMessage msg = mail(subject, kOwnerName, kOwnerAddress, when, text);
            msg.display_to = to;
            return msg;
        };
    BuilderFolder folder;
    folder.name = QStringLiteral("Sent Items");
    folder.messages = {
        sent(QStringLiteral("Re: Spring menu draft"),
             QStringLiteral("Marisol Quint"),
             at(3, 10, 8, 30),
             QStringLiteral("Looks good. The rye should go up by fifty cents.")),
        sent(QStringLiteral("Re: Saturday staff schedule"),
             QStringLiteral("Owen Lark"),
             at(3, 16, 9, 10),
             QStringLiteral("That works, thank you Owen.")),
        sent(QStringLiteral("Flour order for April"),
             QStringLiteral("Priya Venn"),
             at(3, 19, 15, 45),
             QStringLiteral("Please send the usual order for the first week of April.")),
    };
    return folder;
}

inline BuilderFolder deletedFolder() {
    BuilderFolder folder;
    folder.name = QStringLiteral("Deleted Items");
    folder.messages = {
        mail(QStringLiteral("Packaging catalog"),
             QStringLiteral("Box Row Supply"),
             QStringLiteral("news@boxrow.example"),
             at(2, 24, 10, 0),
             QStringLiteral("Our spring packaging catalog is here.")),
        mail(QStringLiteral("Your parking permit renewal"),
             QStringLiteral("Port City Parking"),
             QStringLiteral("permits@portcity.example"),
             at(2, 27, 13, 30),
             QStringLiteral("Your parking permit is due for renewal next month.")),
    };
    return folder;
}

inline BuilderMessage contact(const QString& name,
                              const QString& address,
                              const QString& company,
                              const QString& phone) {
    const qsizetype space = name.indexOf(QLatin1Char(' '));
    BuilderMessage msg;
    msg.message_class = QStringLiteral("IPM.Contact");
    msg.subject = name;
    msg.created = at(1, 12, 9);
    msg.extra_props = {
        {sak::email::kPropIdGivenName,
         sak::email::kPropTypeUnicode,
         builderUnicode(name.left(space))},
        {sak::email::kPropIdSurname,
         sak::email::kPropTypeUnicode,
         builderUnicode(name.mid(space + 1))},
        {sak::email::kPropIdCompanyName, sak::email::kPropTypeUnicode, builderUnicode(company)},
        {sak::email::kPropIdBusinessPhone, sak::email::kPropTypeUnicode, builderUnicode(phone)},
    };
    msg.named_props = {{kSetAddress,
                        sak::pst::kLidEmail1EmailAddress,
                        sak::email::kPropTypeUnicode,
                        builderUnicode(address)}};
    return msg;
}

inline BuilderFolder contactsFolder() {
    BuilderFolder folder;
    folder.name = QStringLiteral("Contacts");
    folder.container_class = QStringLiteral("IPF.Contact");
    folder.messages = {
        contact(QStringLiteral("Marisol Quint"),
                QStringLiteral("marisol@harborlane.example"),
                QStringLiteral("Harbor Lane Bakery"),
                QStringLiteral("555-0134")),
        contact(QStringLiteral("Owen Lark"),
                QStringLiteral("owen@harborlane.example"),
                QStringLiteral("Harbor Lane Bakery"),
                QStringLiteral("555-0135")),
        contact(QStringLiteral("Theo Brandt"),
                QStringLiteral("theo@ovenworks.example"),
                QStringLiteral("Oven Works"),
                QStringLiteral("555-0142")),
        contact(QStringLiteral("Priya Venn"),
                QStringLiteral("billing@millstonefoods.example"),
                QStringLiteral("Millstone Foods"),
                QStringLiteral("555-0167")),
        contact(QStringLiteral("Ines Caldera"),
                QStringLiteral("ines@portmarket.example"),
                QStringLiteral("Port Market"),
                QStringLiteral("555-0188")),
    };
    return folder;
}

inline BuilderMessage event(const QString& subject,
                            const QString& location,
                            const QDateTime& start,
                            int minutes,
                            const QString& notes) {
    BuilderMessage msg;
    msg.message_class = QStringLiteral("IPM.Appointment");
    msg.subject = subject;
    msg.sender_name = kOwnerName;
    msg.sender_email = kOwnerAddress;
    msg.body_plain = notes;
    msg.created = start.addDays(-14);
    msg.named_props = {
        {kSetAppointment,
         sak::pst::kLidAppointmentStartWhole,
         sak::email::kPropTypeSysTime,
         builderFiletime(start)},
        {kSetAppointment,
         sak::pst::kLidAppointmentEndWhole,
         sak::email::kPropTypeSysTime,
         builderFiletime(start.addSecs(static_cast<qint64>(minutes) * 60))},
        {kSetAppointment,
         sak::pst::kLidAppointmentLocation,
         sak::email::kPropTypeUnicode,
         builderUnicode(location)},
        {kSetAppointment,
         sak::pst::kLidAppointmentBusyStatus,
         sak::email::kPropTypeInt32,
         builderInt32(2)},
    };
    return msg;
}

inline constexpr int kStaffMeetingIndex = 2;

/// The staff meeting is the one event that shows a calendar's people and repeats: weekly, with one
/// week cancelled, in the bakery's own time zone. Its wall clock is the start read in UTC-8, which
/// is why this zone: the sample's times are stated in UTC and this keeps them what they say.
inline void addStaffMeetingSeries(BuilderMessage& meeting, const QDateTime& start_utc) {
    constexpr int kOffsetHours = -8;
    constexpr int kWeeksUntilCancelled = 2;
    constexpr int kWeeksInSeries = 12;
    constexpr int kMinutesPerHour = 60;
    const QDateTime wall = start_utc.addSecs(static_cast<qint64>(kOffsetHours) * 3600);
    RecurrenceSpec spec;
    spec.day_or_mask = 1U << (wall.date().dayOfWeek() % 7);
    spec.start_date = wall.date();
    spec.start_minutes = wall.time().hour() * kMinutesPerHour;
    spec.end_minutes = spec.start_minutes + kMinutesPerHour;
    spec.deleted = {wall.date().addDays(7LL * kWeeksUntilCancelled)};
    spec.end_type = kRecurEndAfterCount;
    spec.occurrence_count = kWeeksInSeries;
    spec.end_date = wall.date().addDays(7LL * (kWeeksInSeries - 1));
    meeting.named_props.append({kSetAppointment,
                                sak::pst::kLidAppointmentRecur,
                                sak::email::kPropTypeBinary,
                                buildRecurrenceBlob(spec)});
    meeting.named_props.append({kSetAppointment,
                                sak::pst::kLidTimeZoneStruct,
                                sak::email::kPropTypeBinary,
                                buildTimeZoneStruct(480, 0, {}, {})});
    meeting.named_props.append({kSetAppointment,
                                sak::pst::kLidTimeZoneDescription,
                                sak::email::kPropTypeUnicode,
                                builderUnicode(QStringLiteral("Harbor Lane time"))});

    BuilderRecipient organizer{kOwnerName, kOwnerAddress};
    organizer.organizer = true;
    BuilderRecipient baker{QStringLiteral("Marisol Quint"),
                           QStringLiteral("marisol@harborlane.example")};
    baker.track_status = 3;
    BuilderRecipient driver{QStringLiteral("Owen Lark"), QStringLiteral("owen@harborlane.example")};
    driver.recipient_type = 2;
    driver.track_status = 2;
    meeting.recipients = {organizer, baker, driver};
}

/// Events fall in the month of @p anchor, so the calendar's opening month is not empty whenever the
/// sample is generated.
inline BuilderFolder calendarFolder(QDate anchor) {
    const auto on = [&anchor](int day, int hour) {
        return QDateTime(QDate(anchor.year(), anchor.month(), day), QTime(hour, 0), QTimeZone::UTC);
    };
    BuilderFolder folder;
    folder.name = QStringLiteral("Calendar");
    folder.container_class = QStringLiteral("IPF.Appointment");
    folder.messages = {
        event(QStringLiteral("Oven repair visit"),
              QStringLiteral("Harbor Lane kitchen"),
              on(4, 8),
              90,
              QStringLiteral("Theo from Oven Works. Thermostat and door seal.")),
        event(QStringLiteral("Flour delivery"),
              QStringLiteral("Back door"),
              on(9, 7),
              30,
              QStringLiteral("Millstone Foods, usual order.")),
        event(QStringLiteral("Staff meeting"),
              QStringLiteral("Front of shop"),
              on(12, 15),
              60,
              QStringLiteral("Spring menu and Saturday schedule.")),
        event(QStringLiteral("Health inspection"),
              QStringLiteral("Harbor Lane kitchen"),
              on(17, 10),
              60,
              QStringLiteral("Annual inspection.")),
        event(QStringLiteral("Farmers market setup"),
              QStringLiteral("Port Market, corner booth"),
              on(22, 6),
              120,
              QStringLiteral("Bring the awning and two tables.")),
    };
    addStaffMeetingSeries(folder.messages[kStaffMeetingIndex], on(12, 15));
    return folder;
}

inline BuilderFolder tasksFolder() {
    const auto task = [](const QString& subject, int status, double done, const QDateTime& due) {
        BuilderMessage msg;
        msg.message_class = QStringLiteral("IPM.Task");
        msg.subject = subject;
        msg.created = due.addDays(-10);
        msg.named_props = {
            {kSetTask,
             sak::pst::kLidTaskStatus,
             sak::email::kPropTypeInt32,
             builderInt32(static_cast<uint32_t>(status))},
            {kSetTask,
             sak::pst::kLidTaskPercentComplete,
             sak::email::kPropTypeFloat64,
             builderDouble(done)},
            {kSetTask,
             sak::pst::kLidTaskDueDate,
             sak::email::kPropTypeSysTime,
             builderFiletime(due)},
        };
        return msg;
    };
    BuilderFolder folder;
    folder.name = QStringLiteral("Tasks");
    folder.container_class = QStringLiteral("IPF.Task");
    folder.messages = {task(QStringLiteral("Order spring packaging"), 1, 0.5, at(3, 27, 12)),
                       task(QStringLiteral("Renew parking permit"), 0, 0.0, at(3, 31, 12))};
    return folder;
}

inline BuilderFolder notesFolder() {
    BuilderMessage note;
    note.message_class = QStringLiteral("IPM.StickyNote");
    note.subject = QStringLiteral("Call the sign painter");
    note.body_plain = QStringLiteral("Call the sign painter about the second coat.");
    note.created = at(3, 2, 8);
    note.named_props = {
        {kSetNote, sak::pst::kLidNoteColor, sak::email::kPropTypeInt32, builderInt32(3)}};
    BuilderFolder folder;
    folder.name = QStringLiteral("Notes");
    folder.container_class = QStringLiteral("IPF.StickyNote");
    folder.messages = {note};
    return folder;
}

/// The whole store, with the calendar's events in the month of @p calendar_month.
inline QByteArray build(QDate calendar_month) {
    PstStoreBuilder builder;
    builder.store_name = QStringLiteral("Harbor Lane Bakery");
    builder.ipm_subtree_name = QStringLiteral("Top of Personal Folders");
    return builder.build({inboxFolder(),
                          draftsFolder(),
                          sentFolder(),
                          deletedFolder(),
                          contactsFolder(),
                          calendarFolder(calendar_month),
                          tasksFolder(),
                          notesFolder()});
}

}  // namespace sak::pst_fixture::harbor_lane
