// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace sak::ai {

/// @brief One executable skill: reusable task guidance the model can discover by
/// id/description/triggers and load on demand via the sak_skill tool, instead of
/// every skill body being force-injected into the prompt up front.
///
/// Parsed from a Markdown file with required leading front-matter:
///
///     ---
///     id: package-selection
///     description: Map product names to exact vendor/product package IDs.
///     when_to_use: choosing a package; ambiguous product name; multiple candidates
///     ---
///     # Package Selection Skill
///     ...guidance body...
///
/// Missing or malformed front-matter, invalid UTF-8, and oversized input fail
/// closed with an explicit validation error. A missing title may be derived from
/// the first valid Markdown heading after front-matter; identity and description
/// are never guessed from legacy body text. A valid skill requires non-empty id,
/// title, description, and body.
struct Skill {
    QString id;
    QString title;
    QString description;
    QStringList triggers;  // "when to use" hints advertised for progressive disclosure
    QString body;          // full Markdown guidance, loaded on demand
    QString source_path;
    QString parse_error;   // exact fromMarkdown rejection reason; empty after a valid parse

    [[nodiscard]] bool isValid(QStringList* errors = nullptr) const;

    // Parses a skill from Markdown bytes. `path` is recorded as source_path and
    // names any parse error. Never fails hard: rejected input yields a Skill whose
    // isValid() returns false with the exact parse reason.
    [[nodiscard]] static Skill fromMarkdown(const QByteArray& bytes, const QString& path);

    // Cheap catalog view (id/title/description/triggers) with no body, for
    // advertising the skill to the model without spending the full body's tokens.
    [[nodiscard]] QJsonObject toCatalogJson() const;
};

}  // namespace sak::ai
