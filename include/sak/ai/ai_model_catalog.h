// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

/// @file ai_model_catalog.h
/// @brief The canonical, restricted set of OpenAI models this build offers.
///
/// Product decision (Randy, 2026-07-30): the assistant uses ONLY the GPT-5.6
/// line -- sol, terra, and luna. Both the model picker seed and the model list
/// fetched from the OpenAI API are filtered to exactly these IDs, so no other
/// model can be selected or sent.

#include <QString>
#include <QStringList>

namespace sak {

/// @brief The only OpenAI model IDs the assistant offers, in display order.
/// @note The first entry is the default selection.
inline QStringList supportedOpenAiModels() {
    return {QStringLiteral("gpt-5.6-sol"),
            QStringLiteral("gpt-5.6-terra"),
            QStringLiteral("gpt-5.6-luna")};
}

/// @brief The default model (flagship of the supported line).
inline QString defaultOpenAiModel() {
    return QStringLiteral("gpt-5.6-sol");
}

/// @brief True when @p model is one of the supported GPT-5.6 IDs.
/// @note Trims surrounding whitespace; comparison is exact (case-sensitive,
///       matching the OpenAI model IDs).
inline bool isSupportedOpenAiModel(const QString& model) {
    return supportedOpenAiModels().contains(model.trimmed());
}

}  // namespace sak
