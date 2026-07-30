// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/browser_bridge.h"

#include "sak/win32mcp/browser_contract.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStandardPaths>

#include <cstring>

namespace sak::win32mcp::browser {

namespace {

QString compactJson(const QJsonObject& object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

// A screenshot payload is page-influenced in size (a full-page capture scales with the
// document height). Cap the base64 length so a pathological page cannot flood the model
// context or the transport; a rejected capture returns an honest error, not a silent
// truncation. 16 MiB of base64 is ~12 MiB of PNG -- ample for any real viewport.
constexpr int kMaxScreenshotBase64 = 16 * 1024 * 1024;

// A printed PDF scales with the page; cap the base64 so a pathological document cannot flood
// the transport. 24 MiB of base64 is ~18 MiB of PDF; a rejected print returns an honest error
// telling the caller to narrow it with page_ranges.
constexpr int kMaxPdfBase64 = 24 * 1024 * 1024;

// Where a browser_print PDF lands: the user's Downloads under a SAK-Utility subfolder -- the
// natural, visible place for a saved page. Falls back to the temp dir if Downloads is
// unavailable. The name is timestamped and the bytes are written by US (the extension only
// returns the payload), so a page can steer neither the path nor the filename.
QString printOutputPath() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (dir.isEmpty()) {
        dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    }
    dir = QDir(dir).filePath(QStringLiteral("SAK-Utility"));
    const QString stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss-zzz"));
    return QDir(dir).filePath(QStringLiteral("page-%1.pdf").arg(stamp));
}

// Only image types WE emit are honored; anything else is coerced to PNG so a tampered or
// unexpected mimeType can never turn the image block into an arbitrary content type.
QString sanitizedImageMime(const QString& mime) {
    if (mime == QLatin1String("image/png") || mime == QLatin1String("image/jpeg") ||
        mime == QLatin1String("image/webp")) {
        return mime;
    }
    return QStringLiteral("image/png");
}

// Read the true pixel dimensions from a PNG's IHDR chunk: 8-byte signature, then a chunk
// whose type at offset 12 is "IHDR" with big-endian width@16 and height@20. This is the
// authoritative image size regardless of devicePixelRatio / clip / emulation, and it is a
// bounded, fixed-offset read of the (untrusted) payload. Returns false unless it is a
// valid PNG header with positive dimensions.
bool pngDimensions(const QByteArray& png, int& width, int& height) {
    static const unsigned char kSignature[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (png.size() < 24 || std::memcmp(png.constData(), kSignature, 8) != 0 ||
        std::memcmp(png.constData() + 12, "IHDR", 4) != 0) {
        return false;
    }
    const auto* bytes = reinterpret_cast<const unsigned char*>(png.constData());
    const auto be32 = [bytes](int offset) {
        return (static_cast<quint32>(bytes[offset]) << 24) |
               (static_cast<quint32>(bytes[offset + 1]) << 16) |
               (static_cast<quint32>(bytes[offset + 2]) << 8) |
               static_cast<quint32>(bytes[offset + 3]);
    };
    width = static_cast<int>(be32(16));
    height = static_cast<int>(be32(20));
    return width > 0 && height > 0;
}

// The summary line for a screenshot, using the PNG's own IHDR dimensions when they can be
// read from the base64 (a 64-char prefix decodes to ~48 bytes, past IHDR at offset 24).
QString screenshotSummary(const QString& base64) {
    int width = 0;
    int height = 0;
    const QByteArray head = QByteArray::fromBase64(base64.left(64).toLatin1());
    if (pngDimensions(head, width, height)) {
        return QStringLiteral("Captured a %1x%2 PNG screenshot of the active tab.")
            .arg(width)
            .arg(height);
    }
    return QStringLiteral("Captured a PNG screenshot of the active tab.");
}

QString formatSnapshot(const SnapshotView& view) {
    const QString body = view.outline.isEmpty() ? QStringLiteral("(no interactable elements found)")
                                                : view.outline;
    return QStringLiteral("url: %1\ntitle: %2\nelements: %3\n%4")
        .arg(view.url, view.title)
        .arg(view.element_count)
        .arg(body);
}

// Commands that land the browser on a new document or a different tab, after which
// any ref from a prior snapshot is meaningless. Programmatic navigation keeps CDP
// attached (no onDetached fires), so the bridge must invalidate refs itself.
bool isNavigationCmd(const QString& cmd) {
    return cmd == QLatin1String("navigate") || cmd == QLatin1String("back") ||
           cmd == QLatin1String("forward") || cmd == QLatin1String("reload") ||
           cmd == QLatin1String("selectTab") || cmd == QLatin1String("newTab") ||
           cmd == QLatin1String("closeTab");
}

}  // namespace

void BrowserBridgeSession::onHostConnected() {
    connected_ = true;
    ref_index_ = QJsonObject{};
    ref_index_stale_ = false;
    outstanding_id_.clear();
    outstanding_cmd_.clear();
    ++session_epoch_;  // new browser session: any in-flight reply is for a dead one
}

void BrowserBridgeSession::onHostDisconnected() {
    connected_ = false;
    ref_index_ = QJsonObject{};
    ref_index_stale_ = false;
    outstanding_id_.clear();
    outstanding_cmd_.clear();
    ++session_epoch_;
}

void BrowserBridgeSession::onDetached() {
    ref_index_stale_ = true;
    // A snapshot already in flight was captured against the now-detached DOM; move the
    // epoch so its reply cannot install ref_index or clear the stale flag.
    ++session_epoch_;
}

BrowserBridgeSession::Outgoing BrowserBridgeSession::refuse(const QString& reason) const {
    return {QJsonObject{}, false, reason};
}

BrowserBridgeSession::Outgoing BrowserBridgeSession::beginCommand(const QString& tool,
                                                                  const QJsonObject& arguments) {
    if (!connected_) {
        return refuse(QStringLiteral(
            "Browser not connected: the S.A.K. browser-control extension is not attached."));
    }
    if (!outstanding_id_.isEmpty()) {
        return refuse(QStringLiteral("A browser action is already in progress."));
    }
    if (ref_index_stale_ && arguments.contains(QStringLiteral("ref"))) {
        return refuse(
            QStringLiteral("The page changed since the last snapshot; call browser_snapshot "
                           "again before acting on an element."));
    }
    const ExtensionCommand command = buildExtensionCommand(tool, arguments, ref_index_);
    if (!command.ok) {
        return refuse(command.error);
    }
    const QString id = QStringLiteral("b-%1").arg(++counter_);
    Outgoing out;
    out.ok = true;
    out.frame = command.command;
    out.frame.insert(QStringLiteral("type"), QStringLiteral("command"));
    out.frame.insert(QStringLiteral("id"), id);
    outstanding_id_ = id;
    // Record the cmd WE sent and the epoch it was issued under; state mutations on the
    // reply are keyed to these, never to the untrusted reply frame.
    outstanding_cmd_ = command.command.value(QStringLiteral("cmd")).toString();
    outstanding_epoch_ = session_epoch_;
    return out;
}

void BrowserBridgeSession::fillResult(const QString& sent_cmd,
                                      quint64 sent_epoch,
                                      const QJsonObject& frame,
                                      Incoming& incoming) {
    const QJsonObject payload = frame.value(QStringLiteral("payload")).toObject();
    // Only a reply to a snapshot WE issued may install ref_index -- and only if the
    // browser session has not changed underneath it (no detach/reconnect since we
    // asked), so a snapshot of a now-dead DOM is never trusted for act-by-ref.
    if (sent_cmd == QLatin1String("snapshot")) {
        if (sent_epoch != session_epoch_) {
            incoming.text = QStringLiteral(
                "The page changed while the snapshot was being taken; call browser_snapshot "
                "again.");
            return;
        }
        const SnapshotView view = renderSnapshot(payload);
        ref_index_ = view.ref_index;
        ref_index_stale_ = false;
        incoming.text = formatSnapshot(view);
        return;
    }
    if (sent_cmd == QLatin1String("screenshot")) {
        fillScreenshotResult(payload, incoming);
        return;
    }
    if (sent_cmd == QLatin1String("print")) {
        fillPrintResult(payload, incoming);
        return;
    }
    // After a navigation/tab-change lands, refs from a prior snapshot no longer map to
    // live nodes; refuse act-by-ref until the next snapshot.
    if (isNavigationCmd(sent_cmd)) {
        ref_index_stale_ = true;
    }
    incoming.text = compactJson(payload);
}

void BrowserBridgeSession::fillScreenshotResult(const QJsonObject& payload, Incoming& incoming) {
    const QString data = payload.value(QStringLiteral("data")).toString();
    if (data.isEmpty()) {
        incoming.is_error = true;
        incoming.error = QStringLiteral("The browser returned an empty screenshot.");
        return;
    }
    if (data.size() > kMaxScreenshotBase64) {
        incoming.is_error = true;
        incoming.error = QStringLiteral(
            "The screenshot is too large to return; capture the viewport instead of the "
            "full page.");
        return;
    }
    incoming.image_base64 = data;
    incoming.image_mime = sanitizedImageMime(payload.value(QStringLiteral("mimeType")).toString());
    incoming.text = screenshotSummary(data);
}

// Decode the printed PDF, verify it is a real PDF, and write it to the output folder. The
// extension never touches the filesystem: it returns base64, and the bytes land on disk only
// here, at a path WE choose. A payload that is empty, oversized, or not a "%PDF-" document is
// refused with an honest error rather than written out.
void BrowserBridgeSession::fillPrintResult(const QJsonObject& payload, Incoming& incoming) {
    const QString data = payload.value(QStringLiteral("data")).toString();
    if (data.isEmpty()) {
        incoming.is_error = true;
        incoming.error = QStringLiteral("The browser returned an empty PDF.");
        return;
    }
    if (data.size() > kMaxPdfBase64) {
        incoming.is_error = true;
        incoming.error =
            QStringLiteral("The PDF is too large to save; narrow it with page_ranges.");
        return;
    }
    const QByteArray pdf = QByteArray::fromBase64(data.toLatin1());
    if (!pdf.startsWith("%PDF-")) {
        incoming.is_error = true;
        incoming.error = QStringLiteral("The browser returned data that is not a valid PDF.");
        return;
    }
    const QString path = printOutputPath();
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        incoming.is_error = true;
        incoming.error =
            QStringLiteral("Cannot create the output folder %1.").arg(info.absolutePath());
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(pdf) != pdf.size()) {
        incoming.is_error = true;
        incoming.error = QStringLiteral("Cannot write the PDF to %1.").arg(path);
        return;
    }
    file.close();
    incoming.text =
        compactJson(QJsonObject{{QStringLiteral("ok"), true},
                                {QStringLiteral("saved"), QDir::toNativeSeparators(path)},
                                {QStringLiteral("bytes"), static_cast<int>(pdf.size())}});
}

BrowserBridgeSession::Incoming BrowserBridgeSession::onReply(const QJsonObject& frame) {
    Incoming incoming;
    const QString id = frame.value(QStringLiteral("id")).toString();
    // A frame that does not carry the outstanding id is a late reply from a retired
    // op, an unsolicited event, or a stray -- drop it, never mis-pair it.
    if (outstanding_id_.isEmpty() || id != outstanding_id_) {
        return incoming;  // matched == false
    }
    const QString sent_cmd = outstanding_cmd_;
    const quint64 sent_epoch = outstanding_epoch_;
    outstanding_id_.clear();  // single-winner retire
    outstanding_cmd_.clear();
    incoming.matched = true;

    const QString type = frame.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("result")) {
        fillResult(sent_cmd, sent_epoch, frame, incoming);
        return incoming;
    }
    incoming.is_error = true;
    if (type == QLatin1String("error")) {
        QString message = frame.value(QStringLiteral("message")).toString();
        if (message.isEmpty()) {
            message = frame.value(QStringLiteral("error")).toString();
        }
        incoming.error = message.isEmpty() ? QStringLiteral("Browser command failed.") : message;
    } else {
        incoming.error = QStringLiteral("Unexpected reply type '%1'.").arg(type);
    }
    return incoming;
}

}  // namespace sak::win32mcp::browser
