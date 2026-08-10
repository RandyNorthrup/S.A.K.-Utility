// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_skill.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>

namespace sak::ai {

namespace {

// Skill markdown is small authored content. Fail closed above a generous ceiling
// so an implausibly large file cannot drive unbounded allocation or narrow the
// qsizetype line/column counts used as int indices during parsing.
constexpr qsizetype kMaxSkillBytes = 8 * 1024 * 1024;

QStringList splitTriggers(const QString& value) {
    QStringList triggers;
    const auto parts = value.split(QRegularExpression(QStringLiteral("[;,]")), Qt::SkipEmptyParts);
    for (const auto& part : parts) {
        const QString trimmed = part.trimmed();
        if (!trimmed.isEmpty()) {
            triggers.append(trimmed);
        }
    }
    return triggers;
}

QString titleFromId(const QString& id) {
    QString title = id;
    title.replace(QLatin1Char('-'), QLatin1Char(' '));
    title.replace(QLatin1Char('_'), QLatin1Char(' '));
    if (!title.isEmpty()) {
        title[0] = title[0].toUpper();
    }
    return title;
}

// Applies one "key: value" front-matter line to the skill under construction.
void applyFrontMatterField(Skill* skill, const QString& key, const QString& value) {
    if (key == QLatin1String("id")) {
        skill->id = value;
    } else if (key == QLatin1String("title")) {
        skill->title = value;
    } else if (key == QLatin1String("description")) {
        skill->description = value;
    } else if (key == QLatin1String("when_to_use") || key == QLatin1String("triggers")) {
        skill->triggers = splitTriggers(value);
    }
}

// Consumes a leading "---" front-matter block if present, filling fields on the
// skill and returning the remaining body. Without front-matter the whole text is
// the body.
QString parseFrontMatter(const QString& text, Skill* skill) {
    const QStringList lines = text.split(QLatin1Char('\n'));
    if (lines.isEmpty() || lines.first().trimmed() != QLatin1String("---")) {
        return text;
    }
    int close_index = -1;
    for (int i = 1; i < lines.size(); ++i) {
        if (lines.at(i).trimmed() == QLatin1String("---")) {
            close_index = i;
            break;
        }
    }
    if (close_index < 0) {
        return text;  // unterminated front-matter: treat the whole file as body
    }
    for (int i = 1; i < close_index; ++i) {
        const QString line = lines.at(i);
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0) {
            continue;
        }
        applyFrontMatterField(skill,
                              line.left(colon).trimmed().toLower(),
                              line.mid(colon + 1).trimmed());
    }
    return lines.mid(close_index + 1).join(QLatin1Char('\n'));
}

QString headingText(const QString& trimmed_line) {
    QString heading = trimmed_line;
    while (heading.startsWith(QLatin1Char('#'))) {
        heading.remove(0, 1);
    }
    return heading.trimmed();
}

// A prose line usable as the fallback description (not a heading or list item).
bool looksLikeParagraph(const QString& trimmed_line) {
    return !trimmed_line.startsWith(QLatin1Char('#')) &&
           !trimmed_line.startsWith(QLatin1Char('-')) && !trimmed_line.startsWith(QLatin1Char('*'));
}

// Derives a title/description from Markdown headings/paragraphs when front-matter
// did not supply them.
void deriveMissingFields(Skill* skill, const QString& body) {
    const QStringList lines = body.split(QLatin1Char('\n'));
    for (const auto& line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        if (skill->title.isEmpty() && trimmed.startsWith(QLatin1Char('#'))) {
            skill->title = headingText(trimmed);
            continue;
        }
        if (skill->description.isEmpty() && looksLikeParagraph(trimmed)) {
            skill->description = trimmed;
            break;
        }
    }
    if (skill->title.isEmpty()) {
        skill->title = titleFromId(skill->id);
    }
}

}  // namespace

bool Skill::isValid(QStringList* errors) const {
    bool ok = true;
    if (id.trimmed().isEmpty()) {
        if (errors != nullptr) {
            errors->append(QStringLiteral("Skill %1 is missing an id").arg(source_path));
        }
        ok = false;
    }
    if (body.trimmed().isEmpty()) {
        if (errors != nullptr) {
            errors->append(QStringLiteral("Skill %1 has an empty body").arg(source_path));
        }
        ok = false;
    }
    return ok;
}

Skill Skill::fromMarkdown(const QByteArray& bytes, const QString& path) {
    Skill skill;
    skill.source_path = path;
    skill.id = QFileInfo(path).completeBaseName().trimmed();

    if (bytes.size() > kMaxSkillBytes) {
        // Over the ceiling: return with an empty body so isValid() rejects it,
        // rather than parse an oversized file into the int-indexed line loops.
        return skill;
    }

    QString text = QString::fromUtf8(bytes);
    // Strip a leading UTF-8 BOM (U+FEFF): QString::fromUtf8 keeps it, and it is not
    // whitespace, so an un-stripped BOM would make the first-line "---" front-matter
    // check fail and the whole file (front-matter included) parse as body.
    if (text.startsWith(QChar(0xFEFF))) {
        text.remove(0, 1);
    }
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    QString body = parseFrontMatter(text, &skill);
    if (skill.id.trimmed().isEmpty()) {
        skill.id = QFileInfo(path).completeBaseName().trimmed();
    }
    skill.body = body.trimmed();
    deriveMissingFields(&skill, skill.body);
    return skill;
}

QJsonObject Skill::toCatalogJson() const {
    QJsonObject object;
    object[QStringLiteral("id")] = id;
    object[QStringLiteral("title")] = title;
    object[QStringLiteral("description")] = description;
    QJsonArray trigger_array;
    for (const auto& trigger : triggers) {
        trigger_array.append(trigger);
    }
    object[QStringLiteral("when_to_use")] = trigger_array;
    return object;
}

}  // namespace sak::ai
