// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_tool_loop_detector.h"

#include <algorithm>

namespace sak::ai {

namespace {
constexpr int kArgsPreviewChars = 80;
constexpr char16_t kSignatureSeparator = 0x1F;  // ASCII unit separator
}  // namespace

AiToolLoopDetector::AiToolLoopDetector(int repeat_threshold)
    : m_threshold(std::max(2, repeat_threshold)) {}

QString AiToolLoopDetector::signatureKey(const QString& tool_name, const QString& arguments_json) {
    // Case-insensitive tool name + exact trimmed argument string, joined by a
    // separator that cannot appear in a tool name. A model that is truly stuck
    // re-emits byte-identical arguments; genuinely different arguments stay
    // distinct signatures and never trip the detector.
    return tool_name.trimmed().toLower() + QChar(kSignatureSeparator) + arguments_json.trimmed();
}

bool AiToolLoopDetector::observe(const QString& tool_name, const QString& arguments_json) {
    if (tool_name.trimmed().isEmpty()) {
        return false;
    }
    const QString key = signatureKey(tool_name, arguments_json);
    const int count = m_counts.value(key, 0) + 1;
    m_counts.insert(key, count);
    if (count > m_worst_count) {
        m_worst_count = count;
        m_worst_tool = tool_name.trimmed();
        m_worst_args = arguments_json.trimmed();
    }
    return count >= m_threshold;
}

int AiToolLoopDetector::repeatCountFor(const QString& tool_name,
                                       const QString& arguments_json) const {
    return m_counts.value(signatureKey(tool_name, arguments_json), 0);
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
    m_worst_tool.clear();
    m_worst_args.clear();
    m_worst_count = 0;
}

}  // namespace sak::ai
