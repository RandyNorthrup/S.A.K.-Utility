// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_skill_store.h"

#include "sak/ai/ai_paths.h"
#include "sak/ai/ai_store_file_loader.h"

#include <QDir>

#include <functional>
#include <utility>

namespace sak::ai {

namespace {

constexpr qint64 kMaxSkillFileBytes = 1LL * 1024 * 1024;
constexpr int kMaxSkillFiles = 512;
std::function<void(const QString&)> g_before_file_open_hook;
std::function<void(const QString&)> g_before_file_read_hook;

const detail::StoreFileLoadPolicy& skillFileLoadPolicy() {
    static const detail::StoreFileLoadPolicy policy{QStringLiteral("Skill"),
                                                    QStringLiteral(".md"),
                                                    QStringLiteral("Markdown"),
                                                    kMaxSkillFileBytes,
                                                    kMaxSkillFiles};
    return policy;
}

}  // namespace

void SkillStore::setBeforeFileOpenHookForTesting(std::function<void(const QString&)> hook) {
    g_before_file_open_hook = std::move(hook);
}

void SkillStore::setBeforeFileReadHookForTesting(std::function<void(const QString&)> hook) {
    g_before_file_read_hook = std::move(hook);
}

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
    const detail::StoreFileLoadHooks hooks{g_before_file_open_hook, g_before_file_read_hook};
    const detail::StoreFileConsumer consume = [this, errors](const QString& path,
                                                             const QByteArray& bytes) {
        const Skill skill = Skill::fromMarkdown(bytes, path);
        return addSkill(skill, errors);
    };
    return detail::loadStoreDirectoryFiles(
        directory, skillFileLoadPolicy(), hooks, consume, errors);
}

bool SkillStore::addSkill(const Skill& skill, QStringList* errors) {
    if (!skill.isValid(errors)) {
        return false;
    }
    const auto existing = m_index_by_id.constFind(skill.id);
    if (existing != m_index_by_id.constEnd()) {
        m_skills[*existing] = skill;
    } else {
        m_index_by_id.insert(skill.id, static_cast<int>(m_skills.size()));
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

}  // namespace sak::ai
