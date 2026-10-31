// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_skill.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace sak::ai {

class AiSkillStoreTest;

/// @brief Registry of executable skills loaded from bundled resources and an
/// optional user directory. Mirrors WorkflowStore: built-ins are layered first,
/// then user skills override by id. Used to advertise a cheap skill catalog and
/// to serve full skill bodies on demand through the sak_skill tool.
class SkillStore {
    friend class AiSkillStoreTest;

public:
    SkillStore() = default;

    [[nodiscard]] bool loadDefaults(QStringList* errors = nullptr);
    [[nodiscard]] bool loadBuiltIn(QStringList* errors = nullptr);
    [[nodiscard]] bool loadDirectory(const QString& directory, QStringList* errors = nullptr);
    [[nodiscard]] bool addSkill(const Skill& skill, QStringList* errors = nullptr);

    [[nodiscard]] QVector<Skill> skills() const { return m_skills; }
    [[nodiscard]] const Skill* skillById(const QString& id) const;
    [[nodiscard]] int size() const { return m_skills.size(); }

    [[nodiscard]] static QString builtInResourceRoot();
    [[nodiscard]] static QString defaultUserSkillDirectory();

private:
#ifdef SAK_AI_TEST_HOOKS
    // Test-only (compiled only into tests that define SAK_AI_TEST_HOOKS): race hooks run just
    // before a store file is opened and read.
    static void setBeforeFileOpenHookForTesting(std::function<void(const QString&)> hook);
    static void setBeforeFileReadHookForTesting(std::function<void(const QString&)> hook);
#endif  // SAK_AI_TEST_HOOKS

    QVector<Skill> m_skills;
    QHash<QString, int> m_index_by_id;
};

}  // namespace sak::ai
