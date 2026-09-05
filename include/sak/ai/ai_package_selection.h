// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace sak::ai {

inline constexpr int kDefaultPackageCandidateLimit = 5;

/// Hard input bounds for selectPackageForWorkflow. candidate_limit caps only the REPORTED
/// candidates; these cap the work and retained canonical metadata an attacker-controlled
/// search response can force. Over-sized input is REJECTED, never silently truncated -- a
/// truncated scan could drop the package the user asked for and select a different one.
inline constexpr qsizetype kMaxPackageQueryChars = 256;
inline constexpr qsizetype kMaxPackagesConsidered = 500;
inline constexpr qsizetype kMaxPackageIdChars = 100;
inline constexpr qsizetype kMaxPackageVersionChars = 64;
inline constexpr qsizetype kMaxPackageTitleChars = 256;
inline constexpr qsizetype kMaxPackageDescriptionChars = 2000;

struct AiPackageCandidate {
    /// Canonical bounded fields retained from search input. Unknown source metadata is discarded.
    QString package_id;
    QString version;
    QString title;
    QString description;

    [[nodiscard]] QString displayName() const;
    [[nodiscard]] QJsonObject toJson() const;
};

struct AiPackageSelectionResult {
    bool success{false};
    bool ambiguous{false};
    bool requires_human{false};
    AiPackageCandidate selected;
    QVector<AiPackageCandidate> candidates;
    int rejected_entries{0};
    QString error_message;
    QString question_for_human;

    [[nodiscard]] QJsonObject toJson() const;
};

/// Selects only a raw case-insensitive exact package id or title match. Formatting-normalized
/// collisions never authorize selection. A non-positive candidate limit is invalid and fails
/// closed without exposing any candidate.
[[nodiscard]] AiPackageSelectionResult selectPackageForWorkflow(
    const QString& query,
    const QJsonArray& packages,
    int candidate_limit = kDefaultPackageCandidateLimit);

}  // namespace sak::ai
