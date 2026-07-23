// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_skill.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

namespace sak::ai {

/// @brief Registry of executable skills loaded from bundled resources and an
/// optional user directory. Mirrors WorkflowStore: built-ins are layered first,
/// then user skills override by id. Used to advertise a cheap skill catalog and
/// to serve full skill bodies on demand through the sak_skill tool.
class SkillStore {
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
    void rebuildIndex();

    QVector<Skill> m_skills;
    QHash<QString, int> m_index_by_id;
};

}  // namespace sak::ai
