// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_skill_store.h"

#include "sak/ai/ai_paths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace sak::ai {

namespace {

// A skill body is prose instructions; 1 MiB is far beyond any real skill and bounds
// the per-file allocation from an attacker-writable directory. The file count is
// capped so a directory stuffed with files cannot drive unbounded work/memory.
constexpr qint64 kMaxSkillFileBytes = 1LL * 1024 * 1024;
constexpr int kMaxSkillFiles = 512;

bool loadSkillFile(const QString& path, Skill* skill, QStringList* errors) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errors != nullptr) {
            errors->append(QStringLiteral("%1: %2").arg(path, file.errorString()));
        }
        return false;
    }
    // Bound the allocation before reading the whole file: fail closed on an
    // unreadable size or one exceeding the cap rather than readAll() an attacker
    // sized file into memory.
    const qint64 size = file.size();
    if (size < 0 || size > kMaxSkillFileBytes) {
        if (errors != nullptr) {
            errors->append(
                QStringLiteral("Skill file too large or unreadable (%1 bytes, max %2): %3")
                    .arg(size)
                    .arg(kMaxSkillFileBytes)
                    .arg(path));
        }
        return false;
    }
    const QByteArray bytes = file.readAll();
    // A short/partial read must not be admitted as valid content: surface the error
    // and fail closed instead of parsing a truncated file.
    if (file.error() != QFileDevice::NoError) {
        if (errors != nullptr) {
            errors->append(QStringLiteral("%1: %2").arg(path, file.errorString()));
        }
        return false;
    }
    *skill = Skill::fromMarkdown(bytes, path);
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

    QStringList files = dir.entryList({QStringLiteral("*.md")}, QDir::Files, QDir::Name);
    bool ok = true;
    if (files.size() > kMaxSkillFiles) {
        if (errors != nullptr) {
            errors->append(
                QStringLiteral("Skill directory %1 has %2 files; only the first %3 are loaded")
                    .arg(directory)
                    .arg(files.size())
                    .arg(kMaxSkillFiles));
        }
        files = files.mid(0, kMaxSkillFiles);
        ok = false;
    }
    for (const auto& file_name : files) {
        const QString path = dir.filePath(file_name);
        // Fail closed on a symlink/reparse-point entry: it can redirect the read to a
        // file outside the intended skill directory (a swap between enumeration and
        // open), so never open it as a skill.
        if (QFileInfo(path).isSymLink()) {
            if (errors != nullptr) {
                errors->append(
                    QStringLiteral("Skipping skill via symlink/reparse point: %1").arg(path));
            }
            ok = false;
            continue;
        }
        Skill skill;
        if (!loadSkillFile(path, &skill, errors)) {
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
