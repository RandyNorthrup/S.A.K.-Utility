// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace sak::ai {

inline constexpr int kDefaultPackageCandidateLimit = 5;

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

[[nodiscard]] QString normalizePackageQueryKey(const QString& value);

[[nodiscard]] AiPackageSelectionResult selectPackageForWorkflow(
    const QString& query,
    const QJsonArray& packages,
    int candidate_limit = kDefaultPackageCandidateLimit);

}  // namespace sak::ai
