// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/email_html_sanitizer.h"

#include <QTextBrowser>
#include <QUrl>
#include <QVariant>
#include <QWidget>

/// @file email_safe_text_browser.h
/// @brief The QTextBrowser used to render untrusted email markup in-process.
///
/// A PST/OST/MBOX/EML file is normally obtained from an untrusted source -- that is the whole
/// point of the email inspector -- so a message body is attacker-authored markup. The export
/// writers already treat it that way: html_email_writer strips active content and emits a strict
/// CSP, and pdf_email_writer strips active content and renders through a QTextDocument that
/// refuses every external resource load. The on-screen viewers must not be the weak link, so this
/// gives them the same second layer the PDF writer has.
///
/// Qt's rich text has no script engine, so the exposure is not code execution -- it is resource
/// loading. QTextBrowser resolves <img src=...> (and, on a link activation, the link target)
/// through loadResource(), which will happily read file:/// and UNC paths off the technician's
/// machine and hand the bytes to the document. A message that ships
/// <img src="file:///C:/Users/Username/.ssh/id_rsa"> or a remote tracking pixel would otherwise
/// disclose local content or phone home the moment the message is selected.
///
/// The policy itself is sak::emailResourceIsAllowed() in sak/email_html_sanitizer.h -- a data:
/// whitelist, kept QtCore-only so it is unit-testable without a display.
namespace sak {

/// QTextBrowser for untrusted message bodies: denies every non-data: resource load, and refuses
/// to navigate on a link activation (a click would otherwise make QTextBrowser setSource() the
/// target and render whatever it could read).
class EmailSafeTextBrowser : public QTextBrowser {
public:
    explicit EmailSafeTextBrowser(QWidget* parent = nullptr) : QTextBrowser(parent) {
        setReadOnly(true);
        setOpenExternalLinks(false);
        setOpenLinks(false);
    }

protected:
    QVariant loadResource(int type, const QUrl& name) override {
        if (emailResourceIsAllowed(name)) {
            return QTextBrowser::loadResource(type, name);
        }
        return {};
    }
};

}  // namespace sak
