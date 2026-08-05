// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file backup_file_codec.h
/// @brief Streaming per-file compression and encryption for profile backups.
///
/// A user profile backup routinely runs to tens of gigabytes, so the archive-then-encrypt
/// shape used for application data (Compress-Archive, then read the whole archive into a
/// QByteArray) cannot be reused here: it needs the payload in memory and roughly twice the
/// destination space. This codec instead transforms ONE FILE AT A TIME, streaming, as part
/// of the copy the backup worker already performs.
///
/// Consequences that matter:
///  - Constant memory regardless of file size.
///  - One pass: no intermediate archive, no extra disk headroom.
///  - The directory tree survives, so selective per-folder restore keeps working and a
///    cancelled backup leaves completed files intact.
///  - LIMIT, stated plainly: file NAMES, folder structure and file SIZES stay visible.
///    Only file CONTENT is encrypted. A profile's layout can itself be sensitive, so the
///    wizard says this where the technician chooses the option, not only here.
///
/// Container, written only when compression or encryption is requested (a plain copy stays
/// a plain byte-for-byte copy, so nothing changes for the default path):
///
///     unencrypted:  "SAKBFC1\0" | inner header | payload
///     encrypted:    "SAKBFE1\0" | salt||IV | ENC(inner header | payload) | tag
///
///     inner header: flags uint32 | reserved uint32 | original size int64   16 bytes
///     payload:      raw bytes, or a zlib deflate stream when the compressed flag is set
///
/// Whether a file is encrypted is carried by the magic itself rather than by a cleartext
/// flag, so nothing outside the ciphertext describes how to interpret the payload. The
/// compressed flag and the original size sit INSIDE the encrypted region and are covered
/// by the tag: a cleartext "compressed" bit could be flipped to change how a genuine
/// payload is read while the tag still verified.
///
/// Unencrypted containers carry no tag; their integrity comes from the manifest's SHA-256
/// tree digest, the same protection a verbatim copy has.

#pragma once

#include "error_codes.h"

#include <QString>

#include <expected>
#include <functional>

namespace sak {

/// zlib's own default. Level 0 would store, which the caller should express by turning
/// compression off instead.
inline constexpr int kBackupDefaultCompressionLevel = 6;

/// Read/write granularity. Large enough to keep syscall overhead off a multi-gigabyte
/// copy, small enough that cancellation is observed promptly.
inline constexpr int kBackupCodecChunkBytes = 1 << 20;  // 1 MiB

struct BackupCodecOptions {
    bool compress{false};
    int compression_level{kBackupDefaultCompressionLevel};  // 1-9
    bool encrypt{false};
    QString password;

    /// True when neither transform is requested, i.e. the caller should just copy.
    [[nodiscard]] bool isPassThrough() const { return !compress && !encrypt; }
};

struct BackupCodecResult {
    qint64 plain_bytes{0};   ///< Size of the original file content.
    qint64 stored_bytes{0};  ///< Size actually written, including container overhead.
};

/// @brief Write @p source_path to @p dest_path applying the requested transforms.
/// @param cancelled Polled between chunks; returning true aborts and removes the partial
///        destination, so a cancelled backup never leaves a truncated file behind.
/// @return Byte counts, or an error. Fails closed: encryption requested with an empty
///         password is an error, never a silent plaintext copy.
[[nodiscard]] auto writeBackupFile(const QString& source_path,
                                   const QString& dest_path,
                                   const BackupCodecOptions& options,
                                   const std::function<bool()>& cancelled = {})
    -> std::expected<BackupCodecResult, error_code>;

/// @brief Restore a file written by writeBackupFile().
///
/// A container that is not encrypted decodes with an empty @p password. Plaintext is
/// staged beside the destination and only renamed into place after the tag verifies, so an
/// unauthenticated payload never appears under the final name.
[[nodiscard]] auto readBackupFile(const QString& source_path,
                                  const QString& dest_path,
                                  const QString& password,
                                  const std::function<bool()>& cancelled = {})
    -> std::expected<BackupCodecResult, error_code>;

enum class BackupContainerKind {
    None,       ///< A verbatim copy: no container, restore by plain copy.
    Plain,      ///< Container without encryption (compressed only).
    Encrypted,  ///< Container whose payload needs a password.
};

/// @brief Classify @p path by its magic alone - no password, no payload read.
///
/// This is deliberately the ONLY metadata readable without the password: everything that
/// says how to interpret the payload lives inside the authenticated region. Restore uses
/// it to decide whether to prompt, without having to trust the manifest.
[[nodiscard]] BackupContainerKind backupContainerKind(const QString& path);

}  // namespace sak
