// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/app_action_registry.h"

namespace sak {

QJsonObject appActionDescriptorToJson(const AppActionDescriptor& descriptor) {
    return QJsonObject{
        {QStringLiteral("id"), descriptor.id},
        {QStringLiteral("title"), descriptor.title},
        {QStringLiteral("description"), descriptor.description},
        {QStringLiteral("category"), descriptor.category},
        {QStringLiteral("params"), descriptor.params_schema},
        {QStringLiteral("read_only"), descriptor.read_only},
        {QStringLiteral("mutating"), descriptor.mutating},
        {QStringLiteral("destructive"), descriptor.destructive},
        {QStringLiteral("requires_admin"), descriptor.requires_admin},
    };
}

bool AppActionRegistry::registerAction(const AppActionDescriptor& descriptor,
                                       AppActionInvoke invoke,
                                       QString* error) {
    const QString id = descriptor.id.trimmed();
    if (id.isEmpty()) {
        if (error) {
            *error = QStringLiteral("App action id is empty");
        }
        return false;
    }
    if (!invoke) {
        if (error) {
            *error = QStringLiteral("App action '%1' has no invoke handler").arg(id);
        }
        return false;
    }

    QWriteLocker lock(&m_lock);
    if (m_actions.contains(id)) {
        if (error) {
            *error = QStringLiteral("App action '%1' is already registered").arg(id);
        }
        return false;
    }
    AppActionDescriptor stored = descriptor;
    stored.id = id;
    m_actions.insert(id, Entry{stored, std::move(invoke)});
    if (error) {
        error->clear();
    }
    return true;
}

bool AppActionRegistry::contains(const QString& id) const {
    QReadLocker lock(&m_lock);
    return m_actions.contains(id.trimmed());
}

std::optional<AppActionDescriptor> AppActionRegistry::descriptor(const QString& id) const {
    QReadLocker lock(&m_lock);
    const auto it = m_actions.constFind(id.trimmed());
    if (it == m_actions.constEnd()) {
        return std::nullopt;
    }
    return it->descriptor;
}

QVector<AppActionDescriptor> AppActionRegistry::list() const {
    QReadLocker lock(&m_lock);
    QVector<AppActionDescriptor> out;
    out.reserve(static_cast<int>(m_actions.size()));
    for (const auto& entry : m_actions) {
        out.append(entry.descriptor);
    }
    return out;
}

QJsonArray AppActionRegistry::toJsonCatalog() const {
    QReadLocker lock(&m_lock);
    QJsonArray catalog;
    for (const auto& entry : m_actions) {
        catalog.append(appActionDescriptorToJson(entry.descriptor));
    }
    return catalog;
}

AppActionResult AppActionRegistry::invoke(const QString& id,
                                          const QJsonObject& arguments,
                                          QString* error) const {
    AppActionInvoke handler;
    {
        QReadLocker lock(&m_lock);
        const auto it = m_actions.constFind(id.trimmed());
        if (it == m_actions.constEnd()) {
            if (error) {
                *error = QStringLiteral("Unknown app action: %1").arg(id.trimmed());
            }
            return {false, QStringLiteral("Unknown app action: %1").arg(id.trimmed()), {}};
        }
        handler = it->invoke;  // copy the thunk so we can call it lock-free
    }
    if (error) {
        error->clear();
    }
    // Call outside the lock: the thunk may run for a long time or marshal onto the
    // GUI thread, and must not block concurrent readers.
    return handler(arguments);
}

int AppActionRegistry::count() const {
    QReadLocker lock(&m_lock);
    return static_cast<int>(m_actions.size());
}

}  // namespace sak
