// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_package_selection.h"

#include <QHash>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace sak::ai {
namespace {

constexpr qsizetype kPackageDescriptionPreviewChars = 500;

// Fail closed: a key that is PRESENT but holds a non-string value is MALFORMED input, not a
// reason to fall through to the next alias -- silently taking a different field could turn a
// wrong-typed package_id into an installable id lifted from a display name. Absent (or JSON
// null) genuinely means "not provided" and moves on to the next alias. Returns false when a
// present value is not a string, so the caller can drop the whole record.
bool firstStringValue(const QJsonObject& object,
                      std::initializer_list<const char*> keys,
                      QString* out) {
    out->clear();
    for (const char* key : keys) {
        const QJsonValue value = object.value(QString::fromLatin1(key));
        if (value.isUndefined() || value.isNull()) {
            continue;
        }
        if (!value.isString()) {
            return false;
        }
        const QString text = value.toString().trimmed();
        if (!text.isEmpty()) {
            *out = text;
            return true;
        }
    }
    return true;
}

// Fail closed: validate a candidate id rather than rewrite it. Deleting disallowed
// characters could turn one package id into a DIFFERENT valid id and select the wrong
// package, so an id that is not already well-formed is rejected (returns empty ->
// candidate is dropped by packageCandidatesFromJson). Case is normalized only.
QString safePackageIdToken(const QString& value) {
    const QString out = value.trimmed().toLower();
    static const QRegularExpression kValid(QStringLiteral(R"(^[a-z0-9_.+-]+$)"));
    return kValid.match(out).hasMatch() ? out : QString();
}

// Returns false when the record is malformed (any present field that is not a string). The
// caller drops such a record outright rather than selecting from a half-parsed candidate.
bool candidateFromJson(const QJsonObject& object, AiPackageCandidate* candidate) {
    QString raw_id;
    if (!firstStringValue(object, {"package_id", "id", "package", "name"}, &raw_id) ||
        !firstStringValue(object, {"version"}, &candidate->version) ||
        !firstStringValue(object, {"title", "display_name", "name"}, &candidate->title) ||
        !firstStringValue(object, {"description", "summary"}, &candidate->description)) {
        return false;
    }
    candidate->package_id = safePackageIdToken(raw_id);
    candidate->source = object;
    return true;
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
    QString source_name;
    if (!firstStringValue(candidate.source, {"name", "display_name"}, &source_name)) {
        return false;  // malformed record: never call it an exact match
    }
    return !source_name.isEmpty() && normalizePackageQueryKey(source_name) == query_key;
}

QJsonArray candidatesToJson(const QVector<AiPackageCandidate>& candidates) {
    QJsonArray array;
    for (const auto& candidate : candidates) {
        array.append(candidate.toJson());
    }
    return array;
}

struct ParsedPackages {
    QVector<AiPackageCandidate> m_candidates;
    int m_rejected_entries{0};
    QSet<QString> m_conflicting_ids;
};

ParsedPackages packageCandidatesFromJson(const QJsonArray& packages) {
    ParsedPackages parsed;
    QHash<QString, QString> version_by_id;
    for (const auto& value : packages) {
        AiPackageCandidate candidate;
        if (!value.isObject() || !candidateFromJson(value.toObject(), &candidate) ||
            candidate.package_id.isEmpty()) {
            // Counted, not hidden: the caller reports how many records were refused so a
            // wholly malformed response cannot masquerade as an honest "nothing found".
            ++parsed.m_rejected_entries;
            continue;
        }
        const auto existing = version_by_id.constFind(candidate.package_id);
        if (existing != version_by_id.constEnd()) {
            // Two records claim the same id. Keeping the first silently would install a
            // version the human never saw, so remember the conflict and refuse to
            // auto-select that id below.
            if (*existing != candidate.version) {
                parsed.m_conflicting_ids.insert(candidate.package_id);
            }
            continue;
        }
        version_by_id.insert(candidate.package_id, candidate.version);
        parsed.m_candidates.append(candidate);
    }
    return parsed;
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

AiPackageSelectionResult noPackageCandidatesResult(const QString& clean_query, int rejected) {
    AiPackageSelectionResult result;
    result.error_message =
        clean_query.isEmpty()
            ? QStringLiteral("Package search returned no usable candidates")
            : QStringLiteral("Package search returned no candidates for '%1'").arg(clean_query);
    if (rejected > 0) {
        result.error_message +=
            QStringLiteral(" (%1 malformed search result(s) rejected)").arg(rejected);
    }
    return result;
}

AiPackageSelectionResult refusedPackageSelection(const QString& message) {
    AiPackageSelectionResult result;
    result.error_message = message;
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
        object[QStringLiteral("description")] = description.left(kPackageDescriptionPreviewChars);
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

    // Fail closed on over-sized input rather than traversing and retaining it. Reject, never
    // truncate: a truncated scan could drop the real package and auto-select a different one.
    if (clean_query.size() > kMaxPackageQueryChars) {
        return refusedPackageSelection(
            QStringLiteral("Package query is too long (%1 characters, limit %2)")
                .arg(clean_query.size())
                .arg(kMaxPackageQueryChars));
    }
    if (packages.size() > kMaxPackagesConsidered) {
        return refusedPackageSelection(
            QStringLiteral("Package search returned too many results to evaluate (%1, limit %2)")
                .arg(packages.size())
                .arg(kMaxPackagesConsidered));
    }

    const ParsedPackages parsed = packageCandidatesFromJson(packages);
    result.candidates = limitedCandidates(parsed.m_candidates, limit);

    if (parsed.m_candidates.isEmpty()) {
        return noPackageCandidatesResult(clean_query, parsed.m_rejected_entries);
    }

    // Fail closed: never auto-select a lone candidate that does not exactly match the
    // query -- a fuzzy single search hit could install the wrong package. Only an exact
    // package_id/title/name match is auto-selected below; anything else asks the human.
    const QVector<AiPackageCandidate> exact_matches = exactPackageMatches(query_key,
                                                                          parsed.m_candidates);

    if (exact_matches.size() == 1 &&
        !parsed.m_conflicting_ids.contains(exact_matches.first().package_id)) {
        return selectedPackageResult(exact_matches.first());
    }

    if (exact_matches.size() > 1) {
        result.candidates = limitedCandidates(exact_matches, limit);
    }

    markAmbiguousPackageResult(&result, clean_query);
    return result;
}

}  // namespace sak::ai
