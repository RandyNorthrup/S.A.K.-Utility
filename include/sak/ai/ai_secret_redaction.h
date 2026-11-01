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
/// WHY IT IS ITS OWN LIBRARY (sak_secret_redaction)
/// Two processes need it: the AI layer, before anything is persisted or logged, and the win32 MCP
/// server, before a tool result reaches the model. sak_win32mcp links no other first-party
/// library, so while this lived in sak_ai_core the server kept a THIRD catalogue of its own, and
/// it had the gap this one had already closed: `access_token=`, `refresh_token=` and
/// `AWS_SECRET_ACCESS_KEY=` reached the model through tool output (NEW-10). Qt Core only.
///
/// CredentialStore::redactSecrets and ::redactSecretsInJson remain as facades onto these, so
/// existing callers are unaffected.

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace sak::ai {

/// @brief Does @p key -- a JSON key, an environment variable, a header or a `name=` label --
/// name a secret?
///
/// THE ONE CATALOGUE. The JSON key test, the `name=value` text rule, the trace store and the
/// win32 MCP server all ask this, because separate catalogues had already disagreed: the trace
/// store matched `authorization`, `cookie` and `credential` by substring while the conversation
/// store's redactor knew five whole keys, so `access_token`, `client_secret`, `Authorization` and
/// `Cookie` reached the persisted conversation in clear text.
///
/// The key is split into words at `_`, `-`, `.`, whitespace and camelCase boundaries
/// (`AWS_SECRET_ACCESS_KEY`, `x-api-key`, `clientSecret`), and its HEAD -- the last word, which
/// is the noun the others qualify -- is judged:
///   - a head of password, passwd, pwd, passphrase, secret(s), token, authorization, cookie(s),
///     credential(s), ciphertext, bearer, apikey, accesskey or clientsecret names a secret
///     (`access_token`, `client_secret`, `Proxy-Authorization`, `Set-Cookie`, `Pwd`), while a
///     fact about one does not (`token_count`, `max_tokens`, `PasswordRequired`,
///     `PasswordLastSet`);
///   - a trailing value, header, hash, confirm or confirmation is a container of the noun
///     before it, so `password_hash` and `authorization_header` name a secret too;
///   - a head of key names one only after api, secret, private, subscription, access or account
///     (`api_key`, `X-Api-Key`, `AWS_SECRET_ACCESS_KEY`, `AccessKey`, `AccountKey`), so a bare
///     `key` (a keystroke, a registry key) is not one.
/// `pass` names a secret only as the key's whole final token of letters, digits and `_`
/// (`pass`, `--pass`, `smtp.pass`, `db-pass`): as a compound's head it is ambiguous
/// (`recovery_pass` is an iteration flag). A word must match whole: `secretary` is not `secret`.
[[nodiscard]] bool keyNamesASecret(const QString& key);

/// @brief Replace anything that looks like a credential in @p text.
///
/// Covers vendor-prefixed keys (OpenAI, Context7, GitHub, Slack, AWS, Google, Stripe), bearer
/// tokens across the whole base64/base64url/opaque alphabet, a `Cookie:` header line, the Wi-Fi
/// passphrase `netsh wlan show profile ... key=clear` prints as `Key Content : ...`, and any
/// `name=value` / `name: value` assignment whose name keyNamesASecret() accepts -- environment
/// style (`AWS_SECRET_ACCESS_KEY=...`), query-string style (`?access_token=...`), header style
/// (`Authorization: Basic ...`, which keeps the scheme word) and quoted JSON style -- in bare,
/// double-quoted and single-quoted form. An assignment becomes `name=[redacted]`, except a
/// quoted JSON member (`"token" : "..."`), which keeps its punctuation and becomes
/// `"token" : "[redacted]"`. Ordinary prose is left untouched: a redactor that fires on normal
/// text trains the reader to ignore it.
[[nodiscard]] QString redactSecrets(const QString& text);

/// @brief redactSecrets applied to a JSON document, VALUE BY VALUE.
///
/// Never redact serialized JSON as text. The text rule cannot see JSON escapes: in
/// `{"cmd":"password=\"my secret\""}` the value after `password=` starts with a backslash, so no
/// value form matches and the secret is printed; and a bare value swallows the closing quote of
/// the string it sits in, so the document no longer parses. This walks the tree instead, leaving
/// structure alone:
///   - a value whose KEY names a secret (keyNamesASecret) is replaced wholesale, whatever it
///     holds;
///   - any other string value goes through redactSecrets(), catching a secret embedded in
///     captured stdout;
///   - objects and arrays are walked recursively; non-string scalars are left alone.
[[nodiscard]] QJsonObject redactSecretsInJson(const QJsonObject& object);

/// @brief redactSecretsInJson for a document whose root is an array.
[[nodiscard]] QJsonArray redactSecretsInJson(const QJsonArray& array);

}  // namespace sak::ai
