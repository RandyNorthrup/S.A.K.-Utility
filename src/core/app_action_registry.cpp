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
        {QStringLiteral("catastrophic"), descriptor.catastrophic},
    };
}

namespace {

// Returns the first reason the descriptor/invoke pair may not be registered, or an
// empty string when it is well formed. The read_only contradiction is fatal because
// the assistant's gate treats a read_only action with zero risk flags as ungated
// (isUngatedReadOnlyAction), so a descriptor that is both read_only and
// mutating/destructive/catastrophic is rejected here rather than stored to be misread
// later.
QString registrationError(const QString& id,
                          const AppActionDescriptor& descriptor,
                          const AppActionInvoke& invoke) {
    if (id.isEmpty()) {
        return QStringLiteral("App action id is empty");
    }
    if (!invoke) {
        return QStringLiteral("App action '%1' has no invoke handler").arg(id);
    }
    if (descriptor.read_only &&
        (descriptor.mutating || descriptor.destructive || descriptor.catastrophic)) {
        return QStringLiteral(
                   "App action '%1' is marked read_only but also carries a state-change flag")
            .arg(id);
    }
    return QString();
}

}  // namespace

bool AppActionRegistry::registerAction(const AppActionDescriptor& descriptor,
                                       AppActionInvoke invoke,
                                       QString* error) {
    const QString id = descriptor.id.trimmed();
    const QString invalid = registrationError(id, descriptor, invoke);
    if (!invalid.isEmpty()) {
        if (error != nullptr) {
            *error = invalid;
        }
        return false;
    }

    const QWriteLocker lock(&m_lock);
    if (m_actions.contains(id)) {
        if (error != nullptr) {
            *error = QStringLiteral("App action '%1' is already registered").arg(id);
        }
        return false;
    }
    AppActionDescriptor stored = descriptor;
    stored.id = id;
    m_actions.insert(id, Entry{.descriptor = stored, .invoke = std::move(invoke)});
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool AppActionRegistry::contains(const QString& id) const {
    const QReadLocker lock(&m_lock);
    return m_actions.contains(id.trimmed());
}

std::optional<AppActionDescriptor> AppActionRegistry::descriptor(const QString& id) const {
    const QReadLocker lock(&m_lock);
    const auto it = m_actions.constFind(id.trimmed());
    if (it == m_actions.constEnd()) {
        return std::nullopt;
    }
    return it->descriptor;
}

QVector<AppActionDescriptor> AppActionRegistry::list() const {
    const QReadLocker lock(&m_lock);
    QVector<AppActionDescriptor> out;
    out.reserve(static_cast<int>(m_actions.size()));
    for (const auto& entry : m_actions) {
        out.append(entry.descriptor);
    }
    return out;
}

QJsonArray AppActionRegistry::toJsonCatalog() const {
    const QReadLocker lock(&m_lock);
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
        const QReadLocker lock(&m_lock);
        const auto it = m_actions.constFind(id.trimmed());
        if (it == m_actions.constEnd()) {
            if (error != nullptr) {
                *error = QStringLiteral("Unknown app action: %1").arg(id.trimmed());
            }
            return {.success = false,
                    .message = QStringLiteral("Unknown app action: %1").arg(id.trimmed()),
                    .data = {}};
        }
        handler = it->invoke;  // copy the thunk so we can call it lock-free
    }
    if (error != nullptr) {
        error->clear();
    }
    // Call outside the lock: the thunk may run for a long time or marshal onto the
    // GUI thread, and must not block concurrent readers.
    return handler(arguments);
}

int AppActionRegistry::count() const {
    const QReadLocker lock(&m_lock);
    return static_cast<int>(m_actions.size());
}

}  // namespace sak
