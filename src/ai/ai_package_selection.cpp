// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_package_selection.h"

#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace sak::ai {
namespace {

QString firstStringValue(const QJsonObject& object, std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        const QString value = object.value(QString::fromLatin1(key)).toString().trimmed();
        if (!value.isEmpty()) {
            return value;
        }
    }
    return {};
}

QString safePackageIdToken(const QString& value) {
    QString out = value.trimmed().toLower();
    out.remove(QRegularExpression(QStringLiteral(R"([^a-z0-9_.+-])")));
    return out;
}

AiPackageCandidate candidateFromJson(const QJsonObject& object) {
    AiPackageCandidate candidate;
    candidate.package_id =
        safePackageIdToken(firstStringValue(object, {"package_id", "id", "package", "name"}));
    candidate.version = firstStringValue(object, {"version"});
    candidate.title = firstStringValue(object, {"title", "display_name", "name"});
    candidate.description = firstStringValue(object, {"description", "summary"});
    candidate.source = object;
    return candidate;
}

QString candidateSummary(const AiPackageCandidate& candidate) {
    QString label = candidate.package_id;
    if (!candidate.title.isEmpty() && normalizePackageQueryKey(candidate.title) !=
                                          normalizePackageQueryKey(candidate.package_id)) {
        label += QStringLiteral(" (%1)").arg(candidate.title);
    }
    if (!candidate.version.isEmpty()) {
        label += QStringLiteral(" v%1").arg(candidate.version);
    }
    return label;
}

bool isExactCandidateMatch(const QString& query_key, const AiPackageCandidate& candidate) {
    if (query_key.isEmpty() || candidate.package_id.isEmpty()) {
        return false;
    }
    if (normalizePackageQueryKey(candidate.package_id) == query_key) {
        return true;
    }
    if (!candidate.title.isEmpty() && normalizePackageQueryKey(candidate.title) == query_key) {
        return true;
    }
    const QString source_name = firstStringValue(candidate.source, {"name", "display_name"});
    return !source_name.isEmpty() && normalizePackageQueryKey(source_name) == query_key;
}

QJsonArray candidatesToJson(const QVector<AiPackageCandidate>& candidates) {
    QJsonArray array;
    for (const auto& candidate : candidates) {
        array.append(candidate.toJson());
    }
    return array;
}

QVector<AiPackageCandidate> packageCandidatesFromJson(const QJsonArray& packages) {
    QVector<AiPackageCandidate> candidates;
    QSet<QString> seen_ids;
    for (const auto& value : packages) {
        if (!value.isObject()) {
            continue;
        }
        const AiPackageCandidate candidate = candidateFromJson(value.toObject());
        if (candidate.package_id.isEmpty() || seen_ids.contains(candidate.package_id)) {
            continue;
        }
        seen_ids.insert(candidate.package_id);
        candidates.append(candidate);
    }
    return candidates;
}

QVector<AiPackageCandidate> limitedCandidates(const QVector<AiPackageCandidate>& candidates,
                                              int limit) {
    QVector<AiPackageCandidate> limited;
    for (const auto& candidate : candidates) {
        if (limited.size() >= limit) {
            break;
        }
        limited.append(candidate);
    }
    return limited;
}

QVector<AiPackageCandidate> exactPackageMatches(const QString& query_key,
                                                const QVector<AiPackageCandidate>& candidates) {
    QVector<AiPackageCandidate> matches;
    QSet<QString> seen_ids;
    for (const auto& candidate : candidates) {
        if (!isExactCandidateMatch(query_key, candidate) ||
            seen_ids.contains(candidate.package_id)) {
            continue;
        }
        matches.append(candidate);
        seen_ids.insert(candidate.package_id);
    }
    return matches;
}

AiPackageSelectionResult noPackageCandidatesResult(const QString& clean_query) {
    AiPackageSelectionResult result;
    result.error_message =
        clean_query.isEmpty()
            ? QStringLiteral("Package search returned no usable candidates")
            : QStringLiteral("Package search returned no candidates for '%1'").arg(clean_query);
    return result;
}

AiPackageSelectionResult selectedPackageResult(const AiPackageCandidate& candidate) {
    AiPackageSelectionResult result;
    result.success = true;
    result.selected = candidate;
    return result;
}

void markAmbiguousPackageResult(AiPackageSelectionResult* result, const QString& clean_query) {
    QStringList summaries;
    for (const auto& candidate : result->candidates) {
        summaries << candidate.displayName();
    }
    result->ambiguous = true;
    result->requires_human = true;
    result->error_message =
        clean_query.isEmpty()
            ? QStringLiteral("Ambiguous package match. Choose an exact package_id.")
            : QStringLiteral("Ambiguous package match for '%1'. Choose an exact package_id.")
                  .arg(clean_query);
    result->question_for_human = QStringLiteral("%1 Candidates: %2")
                                     .arg(result->error_message,
                                          summaries.join(QStringLiteral("; ")));
}

}  // namespace

QString AiPackageCandidate::displayName() const {
    return candidateSummary(*this);
}

QJsonObject AiPackageCandidate::toJson() const {
    QJsonObject object = source;
    object[QStringLiteral("package_id")] = package_id;
    if (!version.isEmpty()) {
        object[QStringLiteral("version")] = version;
    }
    if (!title.isEmpty()) {
        object[QStringLiteral("title")] = title;
    }
    if (!description.isEmpty()) {
        object[QStringLiteral("description")] = description.left(500);
    }
    object[QStringLiteral("display_name")] = displayName();
    return object;
}

QJsonObject AiPackageSelectionResult::toJson() const {
    QJsonObject object;
    object[QStringLiteral("success")] = success;
    object[QStringLiteral("ambiguous")] = ambiguous;
    object[QStringLiteral("requires_human")] = requires_human;
    object[QStringLiteral("error_message")] = error_message;
    object[QStringLiteral("question_for_human")] = question_for_human;
    if (!selected.package_id.isEmpty()) {
        object[QStringLiteral("selected")] = selected.toJson();
    }
    object[QStringLiteral("candidates")] = candidatesToJson(candidates);
    return object;
}

QString normalizePackageQueryKey(const QString& value) {
    QString normalized = value.trimmed().toLower();
    normalized.replace(QStringLiteral("++"), QStringLiteral("plusplus"));
    normalized.replace(QStringLiteral("+"), QStringLiteral("plus"));
    normalized.replace(QStringLiteral("#"), QStringLiteral("sharp"));
    normalized.replace(QStringLiteral("&"), QStringLiteral("and"));
    normalized.remove(QRegularExpression(QStringLiteral(R"([^a-z0-9])")));
    return normalized;
}

AiPackageSelectionResult selectPackageForWorkflow(const QString& query,
                                                  const QJsonArray& packages,
                                                  int candidate_limit) {
    AiPackageSelectionResult result;
    const QString clean_query = query.trimmed();
    const QString query_key = normalizePackageQueryKey(clean_query);
    const int limit = std::max(1, candidate_limit);

    const QVector<AiPackageCandidate> all_candidates = packageCandidatesFromJson(packages);
    result.candidates = limitedCandidates(all_candidates, limit);

    if (all_candidates.isEmpty()) {
        return noPackageCandidatesResult(clean_query);
    }

    if (all_candidates.size() == 1) {
        return selectedPackageResult(all_candidates.first());
    }

    const QVector<AiPackageCandidate> exact_matches = exactPackageMatches(query_key,
                                                                          all_candidates);

    if (exact_matches.size() == 1) {
        return selectedPackageResult(exact_matches.first());
    }

    if (exact_matches.size() > 1) {
        result.candidates = limitedCandidates(exact_matches, limit);
    }

    markAmbiguousPackageResult(&result, clean_query);
    return result;
}

}  // namespace sak::ai
