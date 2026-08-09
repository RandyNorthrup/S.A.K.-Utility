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
/// candidates; these cap the work (and the retained source objects) an attacker-controlled
/// search response can force. Over-sized input is REJECTED, never silently truncated -- a
/// truncated scan could drop the package the user asked for and select a different one.
inline constexpr qsizetype kMaxPackageQueryChars = 256;
inline constexpr qsizetype kMaxPackagesConsidered = 500;

struct AiPackageCandidate {
    QString package_id;
    QString version;
    QString title;
    QString description;
    QJsonObject source;

    [[nodiscard]] QString displayName() const;
    [[nodiscard]] QJsonObject toJson() const;
};

struct AiPackageSelectionResult {
    bool success{false};
    bool ambiguous{false};
    bool requires_human{false};
    AiPackageCandidate selected;
    QVector<AiPackageCandidate> candidates;
    QString error_message;
    QString question_for_human;

    [[nodiscard]] QJsonObject toJson() const;
};

/// Case-folds a human package name/id and spells out symbols (++ # &) for comparison.
/// LOSSY BY CONSTRUCTION: the remaining punctuation is removed, so "foo-bar" and "foobar"
/// share a key. Selection therefore never treats a key match as authority on its own --
/// two candidates sharing a key are reported ambiguous for a human to resolve.
[[nodiscard]] QString normalizePackageQueryKey(const QString& value);

[[nodiscard]] AiPackageSelectionResult selectPackageForWorkflow(
    const QString& query,
    const QJsonArray& packages,
    int candidate_limit = kDefaultPackageCandidateLimit);

}  // namespace sak::ai
