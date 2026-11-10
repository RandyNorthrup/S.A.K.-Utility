// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_tool_loop_detector.h"

#include <QCryptographicHash>

#include <algorithm>
#include <limits>

namespace sak::ai {

namespace {
constexpr int kArgsPreviewChars = 80;
// The detector needs at least two identical observations before a repeat can be
// declared, so any caller-supplied threshold is floored here.
constexpr int kMinRepeatThreshold = 2;
}  // namespace

AiToolLoopDetector::AiToolLoopDetector(int repeat_threshold)
    : m_threshold(std::max(kMinRepeatThreshold, repeat_threshold)) {}

QString AiToolLoopDetector::signatureKey(const QString& tool_name, const QString& arguments_json) {
    const QString name = tool_name.trimmed().toLower();
    if (name.isEmpty() || name.size() > kMaxToolNameChars) {
        return {};
    }
    QString args = arguments_json.trimmed();
    // Past a cap, fold the argument body to a fixed-size length+hash digest so an
    // enormous arguments blob cannot pin unbounded memory per key while staying
    // distinct for genuinely different arguments (the loop heuristic is preserved).
    if (args.size() > kMaxSignatureArgChars) {
        const QByteArray digest =
            QCryptographicHash::hash(args.toUtf8(), QCryptographicHash::Sha256).toHex();
        args = QStringLiteral("#%1:%2").arg(args.size()).arg(QString::fromLatin1(digest));
    }
    // Length-prefix the name so no character inside either field can make two distinct pairs
    // concatenate to one signature.
    return QStringLiteral("%1:%2%3").arg(name.size()).arg(name, args);
}

bool AiToolLoopDetector::observe(const QString& tool_name, const QString& arguments_json) {
    const QString key = signatureKey(tool_name, arguments_json);
    if (key.isEmpty()) {
        return false;
    }
    // Cap the number of distinct tracked signatures. A brand-new signature in a
    // saturated map cannot be part of an existing repeat loop (a real loop re-emits
    // an already-tracked key, so it never reaches this branch), so discarding the
    // accumulated singletons is safe for the heuristic and caps memory fail-closed.
    if (!m_counts.contains(key) && m_counts.size() >= kMaxTrackedSignatures) {
        evictOneSignature();
    }
    const int prior_count = m_counts.value(key, 0);
    const int count = prior_count < std::numeric_limits<int>::max() ? prior_count + 1 : prior_count;
    m_counts.insert(key, count);
    if (count == 1) {
        m_singleton_candidates.enqueue(key);
    }
    if (count > m_worst_count) {
        m_worst_count = count;
        m_worst_tool = tool_name.trimmed();
        m_worst_args = arguments_json.trimmed().left(kArgsPreviewChars + 1);
    }
    return count >= m_threshold;
}

int AiToolLoopDetector::repeatCountFor(const QString& tool_name,
                                       const QString& arguments_json) const {
    const QString key = signatureKey(tool_name, arguments_json);
    return key.isEmpty() ? 0 : m_counts.value(key, 0);
}

void AiToolLoopDetector::evictOneSignature() {
    while (!m_singleton_candidates.isEmpty()) {
        const QString candidate = m_singleton_candidates.dequeue();
        if (m_counts.value(candidate, 0) == 1) {
            m_counts.remove(candidate);
            return;
        }
    }
    if (!m_counts.isEmpty()) {
        m_counts.erase(m_counts.begin());
    }
}

QString AiToolLoopDetector::loopingSummary() const {
    if (m_worst_count <= 0) {
        return {};
    }
    QString args_preview = m_worst_args;
    if (args_preview.size() > kArgsPreviewChars) {
        args_preview = args_preview.left(kArgsPreviewChars) + QStringLiteral("...");
    }
    return QStringLiteral("%1(%2) x%3")
        .arg(m_worst_tool, args_preview, QString::number(m_worst_count));
}

void AiToolLoopDetector::reset() {
    m_counts.clear();
    m_singleton_candidates.clear();
    m_worst_tool.clear();
    m_worst_args.clear();
    m_worst_count = 0;
}

}  // namespace sak::ai
