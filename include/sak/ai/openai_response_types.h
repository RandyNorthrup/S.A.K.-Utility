// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_token_usage_tracker.h"

#include <QString>
#include <QVector>

namespace sak::ai {

struct OpenAIFunctionCall {
    QString call_id;
    QString name;
    QString arguments_json;
};

struct OpenAIUrlCitation {
    QString url;
    QString title;
    int start_index{-1};
    int end_index{-1};
};

struct OpenAIFunctionOutput {
    QString call_id;
    QString output;
};

struct OpenAIInputAttachment {
    enum class Type {
        Text,
        Image,
        File,
    };

    Type type{Type::Text};
    QString label;
    QString text;
    QString image_url;
    QString filename;
    QString file_data;
    QString detail{QStringLiteral("auto")};
};

struct OpenAIResponseRequest {
    QString api_key;
    QString model;
    QString instructions;
    QString input;
    QVector<OpenAIInputAttachment> attachments;
    QVector<OpenAIFunctionOutput> function_outputs;
    QString reasoning_effort;
    QString previous_response_id;
    bool enable_web_search{false};
    bool enable_local_tools{false};
};

struct OpenAIResponseResult {
    QString id;
    QString output_text;
    QString raw_json;
    TokenUsage usage;
    QVector<OpenAIFunctionCall> function_calls;
    QVector<OpenAIUrlCitation> citations;
};

}  // namespace sak::ai
