// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QHash>
#include <QString>

namespace sak::ai {

/// @brief Detects a stuck AI tool loop: the model calling the SAME tool with the
/// SAME arguments over and over. The flat per-message turn cap eventually halts a
/// runaway, but a model repeating one identical call burns the whole budget first.
/// This breaks earlier, on the specific signal of an exact-repeat call.
///
/// A "signature" is the (case-normalized) tool name plus its trimmed argument
/// string; distinct arguments are treated as distinct work and never count as a
/// loop. Counting accumulates across turns within one user message; call reset()
/// when a new message begins.
class AiToolLoopDetector {
public:
    static constexpr int kDefaultRepeatThreshold = 4;

    explicit AiToolLoopDetector(int repeat_threshold = kDefaultRepeatThreshold);

    /// Records one requested tool call. Returns true when this call's signature
    /// has now occurred at least the repeat threshold number of times (i.e. the
    /// model is looping on it).
    bool observe(const QString& tool_name, const QString& arguments_json);

    /// True if any signature has reached the repeat threshold.
    [[nodiscard]] bool isLooping() const { return m_worst_count >= m_threshold; }

    /// Occurrences seen so far for a specific tool call signature.
    [[nodiscard]] int repeatCountFor(const QString& tool_name, const QString& arguments_json) const;

    /// Highest repeat count seen for any single signature.
    [[nodiscard]] int maxRepeatCount() const { return m_worst_count; }

    /// Human-readable description of the most-repeated call (name + short args
    /// preview + count), for logs/transcript. Empty before anything is observed.
    [[nodiscard]] QString loopingSummary() const;

    [[nodiscard]] int repeatThreshold() const { return m_threshold; }

    void reset();

private:
    [[nodiscard]] static QString signatureKey(const QString& tool_name,
                                              const QString& arguments_json);

    int m_threshold;
    QHash<QString, int> m_counts;
    QString m_worst_tool;
    QString m_worst_args;
    int m_worst_count{0};
};

}  // namespace sak::ai
