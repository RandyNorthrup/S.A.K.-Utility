// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file dbx_writer.cpp
/// @brief Outlook Express DBX file writer implementation

#include "sak/dbx_writer.h"

#include "sak/logger.h"

#include <QFile>

namespace sak {

// ============================================================================
// Construction / Destruction
// ============================================================================

DbxWriter::DbxWriter(const QString& output_dir) : m_output_dir(output_dir) {}

DbxWriter::~DbxWriter() {
    finalize();
}

// ============================================================================
// Public API
// ============================================================================

std::expected<void, error_code> DbxWriter::writeMessage(
    const PstItemDetail& item,
    const QVector<QPair<QString, QByteArray>>& attachment_data,
    const QString& folder_path) {
    // Fail closed: a conformant Outlook Express OE5/6 .dbx requires the 0x24BC
    // header tree-root plus a B-tree of MessageInfo index nodes to enumerate
    // messages. This writer has no conformant index writer, so no reader (Outlook
    // Express, libdbx) could read anything it emitted. Refuse rather than write a
    // corrupt .dbx a user would trust. The former non-conformant message/header
    // builders were unreachable dead code and have been removed (B7 code-quality).
    Q_UNUSED(item)
    Q_UNUSED(attachment_data)
    Q_UNUSED(folder_path)
    logError(
        "DbxWriter: Outlook Express DBX output is not implemented (no conformant "
        "B-tree index writer); refusing to emit non-conformant .dbx");
    return std::unexpected(error_code::not_implemented);
}

void DbxWriter::finalize() {
    // writeMessage never opens a file (it fails closed), so m_open_files is always
    // empty; this stays defensive so the invariant holds if that ever changes.
    for (auto* file : m_open_files) {
        if (file && file->isOpen()) {
            file->close();
        }
        delete file;
    }
    m_open_files.clear();
}

}  // namespace sak
