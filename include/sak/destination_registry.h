#pragma once

#include <QObject>
#include <QMap>
#include <QTimer>

#include "sak/orchestration_types.h"

namespace sak {

class DestinationRegistry : public QObject {
    Q_OBJECT

public:
    explicit DestinationRegistry(QObject* parent = nullptr);

    void setStaleTimeoutSeconds(int seconds);
    int staleTimeoutSeconds() const;

    void registerDestination(const DestinationPC& destination);
    void updateHealth(const QString& destination_id, const DestinationHealth& health);

    QVector<DestinationPC> destinations() const;
    bool contains(const QString& destination_id) const;

    static bool checkReadiness(const DestinationPC& destination, qint64 required_free_bytes, QString* reason = nullptr);

Q_SIGNALS:
    void destinationRegistered(const DestinationPC& destination);
    void destinationUpdated(const DestinationPC& destination);
    void destinationRemoved(const QString& destination_id);

private Q_SLOTS:
    void pruneStale();

private:
    QMap<QString, DestinationPC> m_destinations;
    QTimer* m_pruneTimer{nullptr};
    int m_staleTimeoutSeconds{30};
};

} // namespace sak
