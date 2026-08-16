// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_attachment_saver.h
/// @brief Shared attachment-saving utilities used by both the
///        email inspector panel and the attachments browser dialog.

#pragma once

#include <QSet>
#include <QString>
#include <QVector>

#include <cstddef>
#include <cstdint>

class QByteArray;

namespace sak {

/// @brief Result of a single attachment save operation.
struct AttachmentSaveResult {
    bool success = false;
    QString saved_path;
    QString error_message;
};

/// @brief Identifies one attachment: the message that owns it and the
///        attachment's index inside that message.
///
/// This pair is what a content request is issued with and what the
/// asynchronous arrival carries back, so it is the only thing that can prove an
/// arrival belongs to the batch that asked for it. A filename cannot: two
/// messages routinely carry attachments of the same name.
struct AttachmentRef {
    uint64_t message_id = 0;
    int index = 0;

    [[nodiscard]] friend bool operator==(const AttachmentRef& lhs, const AttachmentRef& rhs) {
        return lhs.message_id == rhs.message_id && lhs.index == rhs.index;
    }
};

/// Hash for QSet/QHash storage. Found by argument-dependent lookup.
[[nodiscard]] size_t qHash(const AttachmentRef& ref, size_t seed = 0) noexcept;

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
/// Both the email inspector panel and the attachments browser dialog use this
/// to avoid duplicating save-state logic. A batch is defined by the exact set
/// of attachments it asked for, and only an arrival that is still outstanding
/// in that set may be recorded. Without that check any stray delivery -- an
/// inline-image fetch, or a leftover from a previous batch -- is written into
/// one of the batch's slots, saving the wrong payload while the batch still
/// reports a full count.
///
/// Call begin() to start a batch, expects() to test each incoming arrival,
/// recordOne() for the ones it accepts, and isComplete() to know when to show
/// results.
class AttachmentBatchSave {
public:
    /// Start a new batch saving into @p dir the attachments named by @p expected.
    /// @return false when a batch is already running, when @p dir is empty, or
    ///         when @p expected names nothing. No batch starts in those cases
    ///         and the caller must not issue any content requests.
    [[nodiscard]] bool begin(const QString& dir, const QVector<AttachmentRef>& expected);

    /// True when @p ref is one this batch asked for and has not been recorded yet.
    [[nodiscard]] bool expects(const AttachmentRef& ref) const;

    /// Record one incoming attachment. Saves to the directory set by begin().
    /// @return The result of this individual save, or a failed result -- nothing
    ///         written, nothing counted -- when @p ref is not outstanding.
    AttachmentSaveResult recordOne(const AttachmentRef& ref,
                                   const QString& filename,
                                   const QByteArray& data);

    /// Record a failure for one specific attachment this batch asked for.
    /// Symmetric to recordOne(): @p ref must still be outstanding, otherwise nothing
    /// is counted and false is returned. A stray or duplicate failure -- one for an
    /// attachment this batch never requested, or one already recorded -- is ignored,
    /// so an unrelated upstream error can neither inflate this batch's failed count
    /// nor complete it early (the defect the old identity-less recordError had).
    /// @return true if the failure was counted against an outstanding entry.
    bool recordError(const AttachmentRef& ref);

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
    [[nodiscard]] int expectedCount() const { return m_expected_count; }
    [[nodiscard]] const QString& directory() const { return m_dir; }

private:
    QString m_dir;
    QSet<AttachmentRef> m_outstanding;
    int m_expected_count = 0;
    int m_succeeded = 0;
    int m_failed = 0;
};

}  // namespace sak
