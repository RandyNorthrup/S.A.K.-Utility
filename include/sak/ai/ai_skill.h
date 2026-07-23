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
/// Parsed from a Markdown file with optional leading front-matter:
///
///     ---
///     id: package-selection
///     description: Map product names to exact vendor/product package IDs.
///     when_to_use: choosing a package; ambiguous product name; multiple candidates
///     ---
///     # Package Selection Skill
///     ...guidance body...
///
/// Front-matter is optional; missing fields fall back to the file name and the
/// first Markdown heading/paragraph so legacy skill files still load.
struct Skill {
    QString id;
    QString title;
    QString description;
    QStringList triggers;  // "when to use" hints advertised for progressive disclosure
    QString body;          // full Markdown guidance, loaded on demand
    QString source_path;

    [[nodiscard]] bool isValid(QStringList* errors = nullptr) const;

    // Parses a skill from Markdown bytes. `path` supplies the fallback id (file
    // stem) and is recorded as source_path. Never fails hard: an unparseable or
    // empty file yields a Skill whose isValid() returns false.
    [[nodiscard]] static Skill fromMarkdown(const QByteArray& bytes, const QString& path);

    // Cheap catalog view (id/title/description/triggers) with no body, for
    // advertising the skill to the model without spending the full body's tokens.
    [[nodiscard]] QJsonObject toCatalogJson() const;
};

}  // namespace sak::ai
