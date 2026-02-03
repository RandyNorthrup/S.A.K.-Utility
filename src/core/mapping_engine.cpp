#include "sak/mapping_engine.h"

#include "sak/destination_registry.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace sak {

MappingEngine::MappingEngine(QObject* parent)
    : QObject(parent) {
}

void MappingEngine::setStrategy(Strategy strategy) {
    m_strategy = strategy;
}

MappingEngine::Strategy MappingEngine::strategy() const {
    return m_strategy;
}

MappingEngine::DeploymentMapping MappingEngine::createOneToMany(const SourceProfile& source,
                                                                const QVector<DestinationPC>& destinations) {
    DeploymentMapping mapping;
    mapping.type = MappingType::OneToMany;
    mapping.sources = {source};
    mapping.destinations = destinations;

    QString error;
    if (!validateMapping(mapping, error)) {
        Q_EMIT validationError(error);
    } else {
        Q_EMIT mappingReady(mapping);
    }

    return mapping;
}

MappingEngine::DeploymentMapping MappingEngine::createManyToMany(const QVector<SourceProfile>& sources,
                                                                 const QVector<DestinationPC>& destinations) {
    DeploymentMapping mapping;
    mapping.type = MappingType::ManyToMany;
    mapping.sources = sources;
    mapping.destinations = destinations;

    QString error;
    if (!validateMapping(mapping, error)) {
        Q_EMIT validationError(error);
    } else {
        Q_EMIT mappingReady(mapping);
    }

    return mapping;
}

MappingEngine::DeploymentMapping MappingEngine::createCustomMapping(const QVector<SourceProfile>& sources,
                                                                    const QVector<DestinationPC>& destinations,
                                                                    const QMap<QString, QString>& rules) {
    DeploymentMapping mapping;
    mapping.type = MappingType::CustomMapping;
    mapping.sources = sources;
    mapping.destinations = destinations;
    mapping.custom_rules = rules;

    QString error;
    if (!validateMapping(mapping, error)) {
        Q_EMIT validationError(error);
    } else {
        Q_EMIT mappingReady(mapping);
    }

    return mapping;
}

bool MappingEngine::validateMapping(const DeploymentMapping& mapping, QString& errorMessage) const {
    if (mapping.sources.isEmpty()) {
        errorMessage = tr("No source profiles selected");
        return false;
    }

    if (mapping.destinations.isEmpty()) {
        errorMessage = tr("No destination PCs available");
        return false;
    }

    switch (mapping.type) {
    case MappingType::OneToMany:
        if (mapping.sources.size() != 1) {
            errorMessage = tr("One-to-many requires exactly one source");
            return false;
        }
        break;
    case MappingType::ManyToMany:
        if (mapping.sources.size() != mapping.destinations.size()) {
            errorMessage = tr("Many-to-many requires sources and destinations to match in count");
            return false;
        }
        break;
    case MappingType::CustomMapping: {
        if (mapping.custom_rules.isEmpty()) {
            errorMessage = tr("Custom mapping rules are empty");
            return false;
        }

        QSet<QString> sourceNames;
        for (const auto& source : mapping.sources) {
            sourceNames.insert(source.username);
        }

        QSet<QString> destinationIds;
        for (const auto& destination : mapping.destinations) {
            if (!destination.destination_id.isEmpty()) {
                destinationIds.insert(destination.destination_id);
            }
        }

        for (auto it = mapping.custom_rules.constBegin(); it != mapping.custom_rules.constEnd(); ++it) {
            if (!sourceNames.contains(it.key())) {
                errorMessage = tr("Custom mapping references unknown source: %1").arg(it.key());
                return false;
            }
            if (!destinationIds.contains(it.value())) {
                errorMessage = tr("Custom mapping references unknown destination: %1").arg(it.value());
                return false;
            }
        }
        break; }
    }

    return true;
}

bool MappingEngine::checkDiskSpace(const DeploymentMapping& mapping) const {
    QMap<QString, qint64> requiredByDestination;

    if (mapping.type == MappingType::OneToMany) {
        const qint64 required = mapping.sources.first().profile_size_bytes;
        for (const auto& destination : mapping.destinations) {
            requiredByDestination[destination.destination_id] = required;
        }
    } else if (mapping.type == MappingType::ManyToMany) {
        for (int i = 0; i < mapping.sources.size() && i < mapping.destinations.size(); ++i) {
            requiredByDestination[mapping.destinations[i].destination_id] = mapping.sources[i].profile_size_bytes;
        }
    } else {
        for (const auto& source : mapping.sources) {
            const auto destinationId = mapping.custom_rules.value(source.username);
            requiredByDestination[destinationId] += source.profile_size_bytes;
        }
    }

    for (const auto& destination : mapping.destinations) {
        const qint64 required = requiredByDestination.value(destination.destination_id, 0);
        if (required > 0 && destination.health.free_disk_bytes < required) {
            return false;
        }
    }

    return true;
}

bool MappingEngine::checkDestinationReadiness(const DeploymentMapping& mapping) const {
    QMap<QString, qint64> requiredByDestination;

    if (mapping.type == MappingType::OneToMany) {
        const qint64 required = mapping.sources.first().profile_size_bytes;
        for (const auto& destination : mapping.destinations) {
            requiredByDestination[destination.destination_id] = required;
        }
    } else if (mapping.type == MappingType::ManyToMany) {
        for (int i = 0; i < mapping.sources.size() && i < mapping.destinations.size(); ++i) {
            requiredByDestination[mapping.destinations[i].destination_id] = mapping.sources[i].profile_size_bytes;
        }
    } else {
        for (const auto& source : mapping.sources) {
            const auto destinationId = mapping.custom_rules.value(source.username);
            requiredByDestination[destinationId] += source.profile_size_bytes;
        }
    }

    for (const auto& destination : mapping.destinations) {
        QString reason;
        const qint64 required = requiredByDestination.value(destination.destination_id, 0);
        if (!DestinationRegistry::checkReadiness(destination, required, &reason)) {
            return false;
        }
    }

    return true;
}

bool MappingEngine::saveTemplate(const DeploymentMapping& mapping, const QString& filePath) const {
    QJsonObject root;
    root["deployment_id"] = mapping.deployment_id;
    root["type"] = mappingTypeToString(mapping.type);

    QJsonArray sources;
    for (const auto& source : mapping.sources) {
        QJsonObject obj;
        obj["username"] = source.username;
        obj["source_hostname"] = source.source_hostname;
        obj["source_ip"] = source.source_ip;
        obj["profile_size_bytes"] = QString::number(source.profile_size_bytes);
        sources.append(obj);
    }
    root["sources"] = sources;

    QJsonArray destinations;
    for (const auto& destination : mapping.destinations) {
        destinations.append(destination.toJson());
    }
    root["destinations"] = destinations;

    QJsonObject rules;
    for (auto it = mapping.custom_rules.constBegin(); it != mapping.custom_rules.constEnd(); ++it) {
        rules[it.key()] = it.value();
    }
    root["custom_rules"] = rules;

    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }

    QJsonDocument doc(root);
    file.write(doc.toJson(QJsonDocument::Indented));
    return file.commit();
}

MappingEngine::DeploymentMapping MappingEngine::loadTemplate(const QString& filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        Q_EMIT validationError(tr("Unable to open template"));
        return {};
    }

    const auto data = file.readAll();
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        Q_EMIT validationError(tr("Template parse error: %1").arg(error.errorString()));
        return {};
    }

    const auto root = doc.object();
    DeploymentMapping mapping;
    mapping.deployment_id = root.value("deployment_id").toString();
    mapping.type = mappingTypeFromString(root.value("type").toString());

    const auto sources = root.value("sources").toArray();
    for (const auto& sourceValue : sources) {
        const auto obj = sourceValue.toObject();
        SourceProfile source;
        source.username = obj.value("username").toString();
        source.source_hostname = obj.value("source_hostname").toString();
        source.source_ip = obj.value("source_ip").toString();
        source.profile_size_bytes = obj.value("profile_size_bytes").toVariant().toLongLong();
        mapping.sources.push_back(source);
    }

    const auto destinationArray = root.value("destinations").toArray();
    for (const auto& destinationValue : destinationArray) {
        mapping.destinations.push_back(DestinationPC::fromJson(destinationValue.toObject()));
    }

    const auto ruleObject = root.value("custom_rules").toObject();
    for (auto it = ruleObject.begin(); it != ruleObject.end(); ++it) {
        mapping.custom_rules.insert(it.key(), it.value().toString());
    }

    QString validateError;
    if (!validateMapping(mapping, validateError)) {
        Q_EMIT validationError(validateError);
    }

    return mapping;
}

QString MappingEngine::selectDestination(const DeploymentAssignment& assignment,
                                         const QVector<DestinationPC>& destinations,
                                         const QSet<QString>& activeDestinations,
                                         qint64 required_free_bytes) {
    Q_UNUSED(assignment);

    QVector<DestinationPC> candidates;
    candidates.reserve(destinations.size());

    for (const auto& destination : destinations) {
        if (destination.destination_id.isEmpty()) {
            continue;
        }
        if (activeDestinations.contains(destination.destination_id)) {
            continue;
        }
        if (!DestinationRegistry::checkReadiness(destination, required_free_bytes, nullptr)) {
            continue;
        }
        candidates.push_back(destination);
    }

    if (candidates.isEmpty()) {
        return {};
    }

    if (m_strategy == Strategy::RoundRobin) {
        if (m_roundRobinIndex < 0) {
            m_roundRobinIndex = 0;
        }
        const int startIndex = m_roundRobinIndex % candidates.size();
        const auto& chosen = candidates[startIndex];
        m_roundRobinIndex = (startIndex + 1) % candidates.size();
        return chosen.destination_id;
    }

    QString selected;
    qint64 bestFree = -1;
    for (const auto& candidate : candidates) {
        if (candidate.health.free_disk_bytes > bestFree) {
            bestFree = candidate.health.free_disk_bytes;
            selected = candidate.destination_id;
        }
    }

    return selected;
}

QString MappingEngine::mappingTypeToString(MappingType type) {
    switch (type) {
    case MappingType::OneToMany:
        return "one_to_many";
    case MappingType::ManyToMany:
        return "many_to_many";
    case MappingType::CustomMapping:
        return "custom";
    }
    return "one_to_many";
}

MappingEngine::MappingType MappingEngine::mappingTypeFromString(const QString& value) {
    if (value == "many_to_many") {
        return MappingType::ManyToMany;
    }
    if (value == "custom") {
        return MappingType::CustomMapping;
    }
    return MappingType::OneToMany;
}

} // namespace sak
