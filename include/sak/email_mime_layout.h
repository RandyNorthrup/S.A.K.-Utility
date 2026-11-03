// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

/// @file email_mime_layout.h
/// @brief How an exported message's parts nest. The MBOX and EML writers share this.

#include "sak/email_content_id.h"
#include "sak/email_types.h"

#include <QByteArray>
#include <QPair>
#include <QString>
#include <QVector>

#include <algorithm>

namespace sak {

/// The stored details of the attachment named @p name, or null.
[[nodiscard]] inline const PstAttachmentInfo* attachmentInfoNamed(const PstItemDetail& item,
                                                                  const QString& name) {
    const auto match = std::ranges::find_if(item.attachments, [&name](const auto& att) {
        return (att.long_filename.isEmpty() ? att.filename : att.long_filename) == name;
    });
    return match == item.attachments.end() ? nullptr : &*match;
}

/// Whether the message's HTML shows the attachment named @p name. Stores give ordinary attachments
/// a Content-ID too, and those stay attachments.
[[nodiscard]] inline bool attachmentIsShownInBody(const PstItemDetail& item, const QString& name) {
    const PstAttachmentInfo* info = attachmentInfoNamed(item, name);
    return info != nullptr && htmlCitesContentId(item.body_html, usableContentId(info->content_id));
}

/// The multipart containers a message needs, outermost first. Attachments go in multipart/mixed.
/// A picture the HTML shows goes with the HTML in multipart/related, because a reader looks for a
/// cid: reference only there (RFC 2387): in multipart/mixed, Thunderbird showed the picture's alt
/// text, warned of remote content, and put the picture after the message. Plain and HTML are the
/// same message and go in multipart/alternative. A message needing none has no container here.
struct MimeLayout {
    bool mixed = false;
    bool related = false;
    bool alternative = false;
};

[[nodiscard]] inline MimeLayout mimeLayoutFor(
    const PstItemDetail& item, const QVector<QPair<QString, QByteArray>>& attachments) {
    MimeLayout layout;
    layout.alternative = !item.body_plain.isEmpty() && !item.body_html.isEmpty();
    for (const auto& attachment : attachments) {
        if (attachmentIsShownInBody(item, attachment.first)) {
            layout.related = true;
        } else {
            layout.mixed = true;
        }
    }
    return layout;
}

}  // namespace sak
