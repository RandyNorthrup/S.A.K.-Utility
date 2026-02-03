#pragma once

#include <QObject>
#include <QMap>
#include <QSet>
#include <QString>
#include <QVector>

#include "sak/orchestration_types.h"

namespace sak {

class MappingEngine : public QObject {
    Q_OBJECT

public:
    enum class MappingType {
        OneToMany,
        ManyToMany,
        CustomMapping
    };

    enum class Strategy {
        LargestFree,
        RoundRobin
    };

    struct SourceProfile {
        QString username;
        QString source_hostname;
        QString source_ip;
        qint64 profile_size_bytes{0};
    };

    struct DeploymentMapping {
        QString deployment_id;
        MappingType type{MappingType::OneToMany};
        QVector<SourceProfile> sources;
        QVector<DestinationPC> destinations;
        QMap<QString, QString> custom_rules;
    };

    explicit MappingEngine(QObject* parent = nullptr);

    void setStrategy(Strategy strategy);
    Strategy strategy() const;

    DeploymentMapping createOneToMany(const SourceProfile& source,
                                      const QVector<DestinationPC>& destinations);
    DeploymentMapping createManyToMany(const QVector<SourceProfile>& sources,
                                       const QVector<DestinationPC>& destinations);
    DeploymentMapping createCustomMapping(const QVector<SourceProfile>& sources,
                                          const QVector<DestinationPC>& destinations,
                                          const QMap<QString, QString>& rules);

    bool validateMapping(const DeploymentMapping& mapping, QString& errorMessage) const;
    bool checkDiskSpace(const DeploymentMapping& mapping) const;
    bool checkDestinationReadiness(const DeploymentMapping& mapping) const;

    bool saveTemplate(const DeploymentMapping& mapping, const QString& filePath) const;
    DeploymentMapping loadTemplate(const QString& filePath);

    QString selectDestination(const DeploymentAssignment& assignment,
                              const QVector<DestinationPC>& destinations,
                              const QSet<QString>& activeDestinations,
                              qint64 required_free_bytes);

Q_SIGNALS:
    void validationError(const QString& message);
    void mappingReady(const DeploymentMapping& mapping);

private:
    static QString mappingTypeToString(MappingType type);
    static MappingType mappingTypeFromString(const QString& value);

    Strategy m_strategy{Strategy::LargestFree};
    int m_roundRobinIndex{0};
};

} // namespace sak
