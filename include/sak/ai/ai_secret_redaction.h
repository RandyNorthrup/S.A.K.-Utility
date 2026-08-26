// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file ai_secret_redaction.h
/// @brief The one place that decides what a secret looks like.
///
/// WHY THIS IS ITS OWN TRANSLATION UNIT
/// Redaction is pure text work, but it used to live inside CredentialStore, which does DPAPI and
/// file I/O. Anything that merely wanted to scrub a string -- the chat-title generator, the
/// conversation store's persistence boundary -- had to link an encrypted credential store to get
/// at it. The predictable result was a SECOND, weaker secret catalogue written locally instead
/// (ai_chat_title recognised only sk-* keys and let bearer tokens, JWTs and password=... through
/// into PERSISTED chat titles). Splitting the rules out removes the reason to copy them.
///
/// CredentialStore::redactSecrets and ::redactSecretsInJson remain as facades onto these, so
/// existing callers are unaffected.

#pragma once

#include <QJsonObject>
#include <QString>

namespace sak::ai {

/// @brief Replace anything that looks like a credential in @p text.
///
/// Covers vendor-prefixed keys (OpenAI, Context7, GitHub, Slack, AWS, Google, Stripe), bearer
/// tokens across the whole base64/base64url/opaque alphabet, and generic
/// password/passwd/secret/token/api_key assignments in bare, double-quoted and single-quoted
/// form. Ordinary prose is left untouched: a redactor that fires on normal text trains the reader
/// to ignore it.
[[nodiscard]] QString redactSecrets(const QString& text);

/// @brief redactSecrets applied to a JSON document, VALUE BY VALUE.
///
/// Never redact serialized JSON as text: the assignment pattern rewrites `"token":"abc"` to
/// `token=[redacted]`, deleting the quotes and the colon, so the document no longer parses and
/// the record is lost. This walks the tree instead, leaving structure alone:
///   - a value whose KEY names a secret is replaced wholesale, whatever it holds;
///   - any other string value goes through redactSecrets(), catching a secret embedded in
///     captured stdout;
///   - objects and arrays are walked recursively; non-string scalars are left alone.
[[nodiscard]] QJsonObject redactSecretsInJson(const QJsonObject& object);

}  // namespace sak::ai
