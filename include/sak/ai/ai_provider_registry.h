// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace sak::ai {

/// The assistant's provider catalog: the providers.json compiled into the executable
/// (resources/ai.qrc). It is the only copy read -- nothing beside the executable can change which
/// endpoints, tools or MCP commands the assistant uses -- and it is read once, when the registry
/// is constructed, because an embedded resource cannot change while the process runs.
class AiProviderRegistry {
public:
    /// @param app_dir the folder a stdio provider's relative command resolves against and must
    ///        stay inside; empty means the executable's own folder.
    explicit AiProviderRegistry(QString app_dir = {});

    [[nodiscard]] QJsonObject providersObject(QString* error_message = nullptr) const;
    [[nodiscard]] QJsonArray providers(QString* error_message = nullptr) const;
    [[nodiscard]] QJsonObject providerById(const QString& provider_id,
                                           QString* error_message = nullptr) const;
    [[nodiscard]] QJsonObject providerStatus(const QString& provider_id,
                                             QString* error_message = nullptr) const;
    [[nodiscard]] QJsonObject providerStatuses(QString* error_message = nullptr) const;

#ifdef SAK_AI_TEST_HOOKS
    /// Test-only (compiled only into tests that define SAK_AI_TEST_HOOKS): a registry over
    /// @p catalog instead of the embedded one, so the status classifier and the gateway can be
    /// driven with provider entries the shipped catalog does not contain.
    [[nodiscard]] static AiProviderRegistry withCatalogForTesting(QString app_dir,
                                                                  QJsonObject catalog);
#endif

private:
    QString m_app_dir;
    // Declared before m_catalog: the constructor fills both in one read, error first.
    QString m_catalog_error;  // why the embedded catalog could not be read; empty when it was
    QJsonObject m_catalog;
};

}  // namespace sak::ai
