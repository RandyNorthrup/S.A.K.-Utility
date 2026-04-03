// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_attachment_saver.h
/// @brief Shared attachment-saving utilities used by both the
///        email inspector panel and the attachments browser dialog.

#pragma once

#include <QString>

class QByteArray;

namespace sak {

/// @brief Result of a single attachment save operation.
struct AttachmentSaveResult {
    bool success = false;
    QString saved_path;
    QString error_message;
};

/// Sanitize an attachment filename for safe use on Windows filesystems.
/// Strips invalid characters, trailing dots/spaces, and returns
/// "attachment" for empty/blank inputs.
[[nodiscard]] QString sanitizeAttachmentFilename(const QString& filename);

/// Save attachment data to an exact file path.
/// @return Result with success flag and the path used.
[[nodiscard]] AttachmentSaveResult saveAttachmentToPath(const QString& path,
                                                        const QByteArray& data);

/// Save attachment data into a directory with automatic filename
/// sanitization and deduplication (appends _1, _2, etc.).
/// @return Result with success flag and the final deduplicated path.
[[nodiscard]] AttachmentSaveResult saveAttachmentToDirectory(const QString& dir,
                                                             const QString& filename,
                                                             const QByteArray& data);

/// @brief Tracks state for a batch attachment save operation.
///
/// Both the email inspector panel and the attachments browser dialog
/// use this to avoid duplicating save-state logic. Call begin() to
/// start a batch, recordOne() for each attachment content callback,
/// and check isComplete() to know when to show results.
class AttachmentBatchSave {
public:
    /// Start a new batch save to @p dir expecting @p count files.
    void begin(const QString& dir, int count);

    /// Record one incoming attachment. Saves to the directory set by begin().
    /// @return The result of this individual save.
    AttachmentSaveResult recordOne(const QString& filename, const QByteArray& data);

    /// Record a failure that happened upstream (e.g. controller error signal).
    void recordError();

    /// True once all expected attachments have been processed.
    [[nodiscard]] bool isComplete() const;

    /// True if a batch is in progress (begin() called, not yet complete/reset).
    [[nodiscard]] bool isActive() const;

    /// Formatted summary string for status display.
    [[nodiscard]] QString summaryText() const;

    /// Reset to idle state (called automatically when complete,
    /// or manually to cancel).
    void reset();

    [[nodiscard]] int succeeded() const { return m_succeeded; }
    [[nodiscard]] int failed() const { return m_failed; }
    [[nodiscard]] const QString& directory() const { return m_dir; }

private:
    QString m_dir;
    int m_pending = 0;
    int m_succeeded = 0;
    int m_failed = 0;
};

}  // namespace sak
