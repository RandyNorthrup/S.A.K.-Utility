#include "sak/destination_registry.h"

namespace sak {

DestinationRegistry::DestinationRegistry(QObject* parent)
    : QObject(parent)
    , m_pruneTimer(new QTimer(this))
{
    m_pruneTimer->setInterval(5000);
    connect(m_pruneTimer, &QTimer::timeout, this, &DestinationRegistry::pruneStale);
    m_pruneTimer->start();
}

void DestinationRegistry::setStaleTimeoutSeconds(int seconds) {
    m_staleTimeoutSeconds = seconds;
}

int DestinationRegistry::staleTimeoutSeconds() const {
    return m_staleTimeoutSeconds;
}

void DestinationRegistry::registerDestination(const DestinationPC& destination) {
    DestinationPC updated = destination;
    updated.last_seen = QDateTime::currentDateTimeUtc();

    const bool exists = m_destinations.contains(updated.destination_id);
    m_destinations.insert(updated.destination_id, updated);

    if (exists) {
        Q_EMIT destinationUpdated(updated);
    } else {
        Q_EMIT destinationRegistered(updated);
    }
}

void DestinationRegistry::updateHealth(const QString& destination_id, const DestinationHealth& health) {
    if (!m_destinations.contains(destination_id)) {
        return;
    }
    auto updated = m_destinations.value(destination_id);
    updated.health = health;
    updated.last_seen = QDateTime::currentDateTimeUtc();
    m_destinations.insert(destination_id, updated);
    Q_EMIT destinationUpdated(updated);
}

QVector<DestinationPC> DestinationRegistry::destinations() const {
    return m_destinations.values().toVector();
}

bool DestinationRegistry::contains(const QString& destination_id) const {
    return m_destinations.contains(destination_id);
}

bool DestinationRegistry::checkReadiness(const DestinationPC& destination, qint64 required_free_bytes, QString* reason) {
    if (!destination.health.admin_rights) {
        if (reason) {
            *reason = QObject::tr("Admin rights required");
        }
        return false;
    }

    if (!destination.health.sak_service_running) {
        if (reason) {
            *reason = QObject::tr("SAK service not running");
        }
        return false;
    }

    if (required_free_bytes > 0 && destination.health.free_disk_bytes < required_free_bytes) {
        if (reason) {
            *reason = QObject::tr("Insufficient disk space");
        }
        return false;
    }

    if (destination.health.cpu_usage_percent >= 90) {
        if (reason) {
            *reason = QObject::tr("High CPU usage");
        }
        return false;
    }

    if (destination.health.ram_usage_percent >= 90) {
        if (reason) {
            *reason = QObject::tr("High memory usage");
        }
        return false;
    }

    return true;
}

void DestinationRegistry::pruneStale() {
    const auto now = QDateTime::currentDateTimeUtc();
    QList<QString> toRemove;

    for (auto it = m_destinations.cbegin(); it != m_destinations.cend(); ++it) {
        if (it.value().last_seen.secsTo(now) > m_staleTimeoutSeconds) {
            toRemove.append(it.key());
        }
    }

    for (const auto& id : toRemove) {
        m_destinations.remove(id);
        Q_EMIT destinationRemoved(id);
    }
}

} // namespace sak
