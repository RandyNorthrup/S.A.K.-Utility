// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_recovery_engine.h
/// @brief Offline file-level recovery scanner for Partition Manager Data Recovery.

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <cstdint>

namespace sak {

inline constexpr uint64_t kFileRecoveryDefaultMaxScanBytes = 512ULL * 1024ULL * 1024ULL;
inline constexpr uint64_t kFileRecoveryDefaultMaxCandidateBytes = 128ULL * 1024ULL * 1024ULL;
inline constexpr int kFileRecoveryDefaultMaxCandidates = 2048;

struct FileRecoveryCandidate {
    QString id;
    QString format;
    QString extension;
    QByteArray recovered_bytes;
    uint64_t offset_bytes{0};
    uint64_t size_bytes{0};
    QByteArray sha256;
};

struct FileRecoveryScanOptions {
    QString image_path;
    uint64_t max_scan_bytes{kFileRecoveryDefaultMaxScanBytes};
    uint64_t max_candidate_bytes{kFileRecoveryDefaultMaxCandidateBytes};
    int max_candidates{kFileRecoveryDefaultMaxCandidates};
    bool capture_candidate_bytes{false};
};

struct FileRecoveryScanResult {
    QVector<FileRecoveryCandidate> candidates;
    QStringList warnings;
    uint64_t bytes_read{0};
    bool source_opened_read_only{false};
    /// True if the carve was stopped early by the caller's cancel flag (deadline). When true the
    /// candidate list is a partial result -- callers must not report the scan as complete.
    bool scan_cancelled{false};
};

struct FileRecoveryRestoreOptions {
    QString image_path;
    QString destination_directory;
    QVector<FileRecoveryCandidate> candidates;
    uint64_t source_hash_bytes{0};
    bool overwrite_existing{false};
};

struct FileRecoveryRestoreResult {
    QStringList restored_paths;
    QStringList warnings;
    bool source_opened_read_only{false};
    bool source_not_mutated{false};
};

class FileRecoveryEngine {
public:
    /// @param cancel  Optional cooperative-cancel flag polled during the (potentially long)
    ///                signature carve. When it flips true the carve stops and the result's
    ///                scan_cancelled is set. Pass nullptr (the default) for an uncancellable scan.
    [[nodiscard]] static FileRecoveryScanResult scanOfflineImage(
        const FileRecoveryScanOptions& options, const std::atomic<bool>* cancel = nullptr);
    [[nodiscard]] static FileRecoveryRestoreResult restoreCandidates(
        const FileRecoveryRestoreOptions& options);

private:
    FileRecoveryEngine() = default;
};

}  // namespace sak
