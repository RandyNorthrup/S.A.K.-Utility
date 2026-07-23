// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_skill_store.h"

#include "sak/ai/ai_paths.h"

#include <QDir>
#include <QFile>

namespace sak::ai {

namespace {

bool loadSkillFile(const QString& path, Skill* skill, QStringList* errors) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errors != nullptr) {
            errors->append(QStringLiteral("%1: %2").arg(path, file.errorString()));
        }
        return false;
    }
    *skill = Skill::fromMarkdown(file.readAll(), path);
    return skill->isValid(errors);
}

}  // namespace

bool SkillStore::loadDefaults(QStringList* errors) {
    bool ok = loadBuiltIn(errors);
    const QString user_dir = defaultUserSkillDirectory();
    if (QDir(user_dir).exists()) {
        ok = loadDirectory(user_dir, errors) && ok;
    }
    return ok;
}

bool SkillStore::loadBuiltIn(QStringList* errors) {
    return loadDirectory(builtInResourceRoot(), errors);
}

bool SkillStore::loadDirectory(const QString& directory, QStringList* errors) {
    QDir dir(directory);
    if (!dir.exists()) {
        if (errors != nullptr) {
            errors->append(QStringLiteral("Skill directory not found: %1").arg(directory));
        }
        return false;
    }

    const QStringList files = dir.entryList({QStringLiteral("*.md")}, QDir::Files, QDir::Name);
    bool ok = true;
    for (const auto& file_name : files) {
        Skill skill;
        if (!loadSkillFile(dir.filePath(file_name), &skill, errors)) {
            ok = false;
            continue;
        }
        ok = addSkill(skill, errors) && ok;
    }
    rebuildIndex();
    return ok;
}

bool SkillStore::addSkill(const Skill& skill, QStringList* errors) {
    if (!skill.isValid(errors)) {
        return false;
    }
    const auto existing = m_index_by_id.constFind(skill.id);
    if (existing != m_index_by_id.constEnd()) {
        m_skills[*existing] = skill;
    } else {
        m_index_by_id.insert(skill.id, m_skills.size());
        m_skills.append(skill);
    }
    return true;
}

const Skill* SkillStore::skillById(const QString& id) const {
    const auto existing = m_index_by_id.constFind(id.trimmed());
    if (existing == m_index_by_id.constEnd()) {
        return nullptr;
    }
    const int index = *existing;
    if (index < 0 || index >= m_skills.size()) {
        return nullptr;
    }
    return &m_skills[index];
}

QString SkillStore::builtInResourceRoot() {
    return QStringLiteral(":/ai/skills");
}

QString SkillStore::defaultUserSkillDirectory() {
    return skillLibraryDirectory();
}

void SkillStore::rebuildIndex() {
    m_index_by_id.clear();
    for (int i = 0; i < m_skills.size(); ++i) {
        m_index_by_id.insert(m_skills.at(i).id, i);
    }
}

}  // namespace sak::ai
