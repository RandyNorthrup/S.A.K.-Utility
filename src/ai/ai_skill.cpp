// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_skill.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QStringConverter>

namespace sak::ai {

namespace {

// Skill markdown is small authored content. Fail closed above a generous ceiling
// so an implausibly large file cannot drive unbounded allocation or narrow the
// qsizetype line/column counts used as int indices during parsing.
constexpr qsizetype kMaxSkillBytes = 8 * 1024 * 1024;
constexpr qsizetype kMarkdownFenceMarkerLength = 3;

// UTF-8 byte-order mark (U+FEFF): QString::fromUtf8 preserves it, so it is stripped explicitly.
constexpr int kByteOrderMark = 0xFEFF;

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

void setParseError(Skill* skill, const QString& detail) {
    skill->parse_error = QStringLiteral("Skill %1 %2").arg(skill->source_path, detail);
}

bool applyFrontMatterLines(const QStringList& lines, int close_index, Skill* skill) {
    for (int i = 1; i < close_index; ++i) {
        const QString line = lines.at(i);
        if (line.trimmed().isEmpty()) {
            continue;
        }
        const int colon = static_cast<int>(line.indexOf(QLatin1Char(':')));
        if (colon <= 0) {
            setParseError(skill, QStringLiteral("has invalid front-matter at line %1").arg(i + 1));
            return false;
        }
        const QString key = line.left(colon).trimmed().toLower();
        QString authored_value = line.mid(colon + 1);
        // Permit the conventional single separator space after ':', but reject
        // any additional id padding instead of silently normalizing its index key.
        if (authored_value.startsWith(QLatin1Char(' '))) {
            authored_value.remove(0, 1);
        }
        if (key == QLatin1String("id") && authored_value != authored_value.trimmed()) {
            setParseError(skill, QStringLiteral("id has leading or trailing whitespace"));
            return false;
        }
        applyFrontMatterField(skill, key, authored_value.trimmed());
    }
    return true;
}

// Consumes the required leading "---" front-matter block, filling fields on the
// skill and returning the remaining body. Any malformed envelope fails closed.
QString parseFrontMatter(const QString& text, Skill* skill) {
    const QStringList lines = text.split(QLatin1Char('\n'));
    if (lines.isEmpty() || lines.first().trimmed() != QLatin1String("---")) {
        setParseError(skill, QStringLiteral("is missing required leading front-matter"));
        return {};
    }
    int close_index = -1;
    for (int i = 1; i < lines.size(); ++i) {
        if (lines.at(i).trimmed() == QLatin1String("---")) {
            close_index = i;
            break;
        }
    }
    if (close_index < 0) {
        setParseError(skill, QStringLiteral("has unterminated front-matter"));
        return {};
    }
    if (!applyFrontMatterLines(lines, close_index, skill)) {
        return {};
    }
    return lines.mid(close_index + 1).join(QLatin1Char('\n'));
}

QString firstMarkdownHeading(const QString& body) {
    static const QRegularExpression kHeading(
        QStringLiteral("^#{1,6}[\\t ]+(.+?)(?:[\\t ]+#+[\\t ]*)?$"));
    bool in_fence = false;
    bool in_comment = false;
    QString fence_marker;
    for (const auto& line : body.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (in_fence) {
            if (trimmed.startsWith(fence_marker)) {
                in_fence = false;
            }
            continue;
        }
        if (trimmed.startsWith(QLatin1String("```")) || trimmed.startsWith(QLatin1String("~~~"))) {
            fence_marker = trimmed.left(kMarkdownFenceMarkerLength);
            in_fence = true;
            continue;
        }
        if (in_comment) {
            if (trimmed.contains(QLatin1String("-->"))) {
                in_comment = false;
            }
            continue;
        }
        if (trimmed.startsWith(QLatin1String("<!--"))) {
            in_comment = !trimmed.contains(QLatin1String("-->"));
            continue;
        }
        const auto match = kHeading.match(trimmed);
        if (match.hasMatch()) {
            return match.captured(1).trimmed();
        }
    }
    return {};
}

// Bundled skills carry explicit identity/description metadata but derive their
// display title from the first real body heading when `title` is omitted.
void deriveMissingFields(Skill* skill, const QString& body) {
    if (skill->title.isEmpty()) {
        skill->title = firstMarkdownHeading(body);
    }
}

bool requireSkillField(const QString& value, const QString& error, QStringList* errors) {
    if (!value.trimmed().isEmpty()) {
        return true;
    }
    if (errors != nullptr) {
        errors->append(error);
    }
    return false;
}

}  // namespace

bool Skill::isValid(QStringList* errors) const {
    if (!parse_error.isEmpty()) {
        if (errors != nullptr) {
            errors->append(parse_error);
        }
        return false;
    }
    bool ok = true;
    if (id.trimmed().isEmpty()) {
        if (errors != nullptr) {
            errors->append(QStringLiteral("Skill %1 is missing an id").arg(source_path));
        }
        ok = false;
    } else if (id != id.trimmed()) {
        if (errors != nullptr) {
            errors->append(
                QStringLiteral("Skill %1 id has leading or trailing whitespace").arg(source_path));
        }
        ok = false;
    }
    ok = requireSkillField(title,
                           QStringLiteral("Skill %1 is missing a title").arg(source_path),
                           errors) &&
         ok;
    ok = requireSkillField(description,
                           QStringLiteral("Skill %1 is missing a description").arg(source_path),
                           errors) &&
         ok;
    ok = requireSkillField(body,
                           QStringLiteral("Skill %1 has an empty body").arg(source_path),
                           errors) &&
         ok;
    return ok;
}

Skill Skill::fromMarkdown(const QByteArray& bytes, const QString& path) {
    Skill skill;
    skill.source_path = path;

    if (bytes.size() > kMaxSkillBytes) {
        skill.parse_error = QStringLiteral("Skill %1 exceeds parser size limit (%2 bytes, max %3)")
                                .arg(path)
                                .arg(bytes.size())
                                .arg(kMaxSkillBytes);
        return skill;
    }

    QStringDecoder decoder(QStringConverter::Utf8);
    QString text = decoder.decode(bytes);
    if (decoder.hasError()) {
        skill.parse_error = QStringLiteral("Skill %1 is not valid UTF-8").arg(path);
        return skill;
    }
    // Strip a leading UTF-8 BOM (U+FEFF): QString::fromUtf8 keeps it, and it is not
    // whitespace, so an un-stripped BOM would make the first-line "---" front-matter
    // check fail and the whole file (front-matter included) parse as body.
    if (text.startsWith(QChar(kByteOrderMark))) {
        text.remove(0, 1);
    }
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    const QString body = parseFrontMatter(text, &skill);
    if (!skill.parse_error.isEmpty()) {
        return skill;
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
